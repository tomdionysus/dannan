//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/listener.h"

#include <sys/socket.h>

#include <atomic>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <stdexcept>
#include <system_error>

#include "dannan/listen/detail.h"

namespace dannan::listen {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

// The hosted system's hand-up behind a gate stop_accepting() shuts, so nothing is
// handed up once it has returned, whether or not the acceptor's close has run yet.
// Kept inside the std::function and found again by its type, so the shared state's
// layout (an installed header's) is unchanged within the minor.
struct Gate {
  // Whether the accept loop has begun: claimed once, by the loop's first step or by
  // a stop_accepting() that came first (Lugh: a node whose start failed returns
  // before its io context ever runs).
  enum Loop : int { waiting, running, never };
  std::shared_ptr<std::atomic<bool>> shut;
  std::shared_ptr<std::atomic<int>> loop;
  std::function<void(std::shared_ptr<Stream>)> accepted;
  void operator()(std::shared_ptr<Stream> stream) const {
    if (shut->load()) return stream->abort();
    if (accepted) accepted(std::move(stream));
  }
};

bool gate_shut(const ListenerShared& shared) {
  const auto* gate = shared.accepted.target<Gate>();
  return gate && gate->shut->load();
}

}  // namespace

std::shared_ptr<detail::ServerTls> ListenerShared::current_tls() {
  if (!config.certificates) return nullptr;
  const auto generation = config.certificates->generation();
  const auto material = config.certificates->current();
  contract::Lock lock(mutex);
  if (!tls || tls_generation != generation) {
    tls = detail::server_tls(*material, config.tls, config.name);
    tls_generation = generation;
  }
  return tls;
}

StreamListener::StreamListener(asio::io_context& io, ListenerConfig config, const contract::TimeSource* time)
    : _shared(std::make_shared<ListenerShared>(io, std::move(config), time)) {
  if (_shared->config.accept_rate) {
    contract::Lock lock(_shared->mutex);
    _shared->accept_bucket.emplace(*_shared->config.accept_rate, time->now());
  }
}

StreamListener::~StreamListener() { stop(); }

const ListenerConfig& StreamListener::config() const noexcept { return _shared->config; }

void StreamListener::start(asio::any_io_executor executor, Accepted accepted) {
  // Fails here, not at the first connection, for TLS that cannot be set up.
  (void)_shared->current_tls();
  _shared->executor = std::move(executor);
  auto loop = std::make_shared<std::atomic<int>>(Gate::waiting);
  _shared->accepted = Gate{std::make_shared<std::atomic<bool>>(false), loop, std::move(accepted)};
  auto& acceptor = _shared->acceptor;
  const auto& settings = _shared->config;
  if (settings.inherited) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (::getsockname(*settings.inherited, reinterpret_cast<sockaddr*>(&address), &length) != 0)
      throw std::system_error(errno, std::generic_category(), settings.name + ": the inherited socket");
    acceptor.assign(address.ss_family == AF_INET6 ? tcp::v6() : tcp::v4(), *settings.inherited);
  } else {
    const tcp::endpoint endpoint(asio::ip::make_address(settings.host), settings.port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
  }
  _port = acceptor.local_endpoint().port();
  _accepting = true;
  asio::post(acceptor.get_executor(), [shared = _shared, loop] {
    // A stop_accepting() that came first has closed the acceptor itself.
    int expected = Gate::waiting;
    if (!loop->compare_exchange_strong(expected, Gate::running)) return;
    accept(shared);
  });
}

void StreamListener::accept(const std::shared_ptr<ListenerShared>& shared) {
  const auto executor = shared->config.strand_per_stream ? asio::any_io_executor(asio::make_strand(shared->executor)) : shared->executor;
  shared->acceptor.async_accept(executor, [shared](const boost::system::error_code& error, tcp::socket socket) {
    if (!shared->acceptor.is_open()) return;
    if (!error) served(shared, std::move(socket));
    // A failed accept (a descriptor short, say) is that connection's alone.
    accept(shared);
  });
}

void StreamListener::served(const std::shared_ptr<ListenerShared>& shared, tcp::socket socket) {
  boost::system::error_code ignored;
  // Accepted before the acceptor's close ran, after stop_accepting() returned.
  if (gate_shut(*shared)) {
    socket.close(ignored);
    return;
  }
  const auto peer = socket.remote_endpoint(ignored);
  const auto& settings = shared->config;
  // What refuses at accept closes at once: a hosted system that answers its own
  // "busy" sets these only as a backstop above its own.
  const auto refuse = [&](uint64_t ListenerCounters::* counter) {
    {
      contract::Lock lock(shared->mutex);
      ++(shared->counters.*counter);
    }
    socket.close(ignored);
  };
  {
    contract::Lock lock(shared->mutex);
    if (settings.max_connections && shared->counters.open >= *settings.max_connections) {
      ++shared->counters.refused_cap;
      socket.close(ignored);
      return;
    }
    if (shared->accept_bucket && !shared->accept_bucket->take(shared->time->now())) {
      ++shared->counters.refused_rate;
      socket.close(ignored);
      return;
    }
  }
  if (settings.limits) {
    switch (settings.limits->admit(peer.address())) {
      case Limits::Verdict::admitted:
        break;
      case Limits::Verdict::address:
        return refuse(&ListenerCounters::refused_address);
      case Limits::Verdict::caller:
        return refuse(&ListenerCounters::refused_caller);
    }
  }
  // Every public protocol's small packets go out at once (Lir: Nagle behind the
  // peer's delayed ACK cost an MQTT PUBACK about 40 ms); Stream::set_no_delay undoes it.
  socket.set_option(tcp::no_delay(true), ignored);
  auto stream = std::make_shared<Stream>(Stream::Private{}, std::move(socket), shared);
  {
    contract::Lock lock(shared->mutex);
    ++shared->counters.accepted;
    ++shared->counters.open;
    std::erase_if(shared->open, [](const std::weak_ptr<Stream>& each) { return each.expired(); });
    shared->open.push_back(stream);
  }
  const auto hand_up = [shared, stream] {
    if (shared->accepted) shared->accepted(stream);
  };
  if (settings.certificates && !settings.start_tls) {
    // On the stream's strand: the handshake, then the hosted system.
    asio::post(stream->get_executor(), [stream, hand_up] {
      stream->handshake([hand_up](const std::string& why) {
        if (why.empty()) hand_up();
      });
    });
  } else {
    asio::post(stream->get_executor(), hand_up);
  }
}

void StreamListener::stop_accepting() {
  // Once: a later call (the destructor's, after stop()) waits on nothing, so a
  // listener outliving its io context's run does not hang.
  if (!_accepting.exchange(false)) return;
  auto shared = _shared;
  const auto close = [shared] {
    boost::system::error_code ignored;
    shared->acceptor.close(ignored);
  };
  // Nothing is handed up after this, whenever the acceptor's close runs.
  if (auto* gate = shared->accepted.target<Gate>()) {
    gate->shut->store(true);
    // The accept loop never began (its io context has not run since start(), Lugh's
    // failed start): it never will touch the acceptor now, so it is closed here.
    int expected = Gate::waiting;
    if (gate->loop->compare_exchange_strong(expected, Gate::never)) return close();
  }
  // The acceptor's strand is on the io context the listener was made with; checked,
  // not assumed (Lir's review), and without one the bounded wait below is taken.
  const auto executor = shared->acceptor.get_executor();
  const auto* strand = executor.target<asio::strand<asio::io_context::executor_type>>();
  asio::io_context* io = strand ? &strand->get_inner_executor().context() : nullptr;
  // Lugh's review: waiting on the acceptor's strand deadlocked where nothing else
  // could run it. With the io context stopped (a listener destroyed after it), no
  // handler runs beside this: closed here. A context stopped for want of work and
  // later restarted on another thread would race this close (Lir's review): a
  // listener is stopped before its io context is run again, not during.
  if (io && io->stopped()) return close();
  // Called on a thread running the io context (a signal handler there, a one-thread
  // context): the close goes on the strand, at once if this is it, and is not waited
  // for; until it runs a connection may reach the backlog, and is closed unserved.
  if (io && io->get_executor().running_in_this_thread()) {
    asio::dispatch(shared->acceptor.get_executor(), close);
    return;
  }
  // From any other thread the acceptor is closed when this returns, so a connect
  // after it is refused; within a bound, as no call waits without one.
  auto closed = std::make_shared<std::promise<void>>();
  auto done = closed->get_future();
  asio::post(shared->acceptor.get_executor(), [close, closed] {
    close();
    closed->set_value();
  });
  (void)done.wait_for(std::chrono::seconds(5));
}

void StreamListener::close_all() { close_all({}, {}); }

void StreamListener::close_all(asio::any_io_executor executor, std::function<void()> done) {
  detail::close_open(*_shared, std::move(executor), std::move(done));
}

namespace detail {

void close_open(ListenerShared& shared, asio::any_io_executor executor, std::function<void()> done) {
  std::vector<std::shared_ptr<Stream>> open;
  {
    contract::Lock lock(shared.mutex);
    for (const auto& each : shared.open)
      if (auto stream = each.lock()) open.push_back(std::move(stream));
    shared.open.clear();
  }
  if (done) {
    if (open.empty()) {
      asio::post(executor, std::move(done));
    } else {
      // Told once, when the last of them has finished.
      auto left = std::make_shared<std::atomic<size_t>>(open.size());
      auto once = std::make_shared<std::function<void()>>(std::move(done));
      for (auto& stream : open)
        stream->when_finished([left, once, executor] {
          if (--*left == 0) asio::post(executor, *once);
        });
    }
  }
  for (auto& stream : open) stream->close();
}

}  // namespace detail

void StreamListener::stop() {
  stop_accepting();
  close_all();
}

ListenerCounters StreamListener::counters() const {
  contract::Lock lock(_shared->mutex);
  return _shared->counters;
}

}  // namespace dannan::listen
