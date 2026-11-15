//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/listener.h"

#include <sys/socket.h>

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <future>
#include <stdexcept>
#include <system_error>

#include "dannan/listen/detail.h"

namespace dannan::listen {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

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
  _shared->accepted = std::move(accepted);
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
  asio::post(acceptor.get_executor(), [shared = _shared] { accept(shared); });
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
  auto& acceptor = _shared->acceptor;
  // On the acceptor's strand, and done when this returns: no connection after it.
  std::promise<void> closed;
  auto done = closed.get_future();
  asio::post(acceptor.get_executor(), [&acceptor, &closed] {
    boost::system::error_code ignored;
    acceptor.close(ignored);
    closed.set_value();
  });
  done.wait();
}

void StreamListener::close_all() { close_all({}, {}); }

void StreamListener::close_all(asio::any_io_executor executor, std::function<void()> done) {
  std::vector<std::shared_ptr<Stream>> open;
  {
    contract::Lock lock(_shared->mutex);
    for (const auto& each : _shared->open)
      if (auto stream = each.lock()) open.push_back(std::move(stream));
    _shared->open.clear();
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

void StreamListener::stop() {
  stop_accepting();
  close_all();
}

ListenerCounters StreamListener::counters() const {
  contract::Lock lock(_shared->mutex);
  return _shared->counters;
}

}  // namespace dannan::listen
