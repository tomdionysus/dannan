//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/connector.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <stdexcept>

#include "dannan/listen/detail.h"

namespace dannan::listen {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = asio::ip::tcp;

namespace {

std::shared_ptr<X509_STORE> load_store(const std::filesystem::path& bundle) {
  std::shared_ptr<X509_STORE> store(X509_STORE_new(), X509_STORE_free);
  if (!store) throw std::runtime_error("cannot make a certificate store");
  if (bundle.empty()) {
    if (X509_STORE_set_default_paths(store.get()) != 1) {
      ERR_clear_error();
      throw std::runtime_error("the system's certificate store cannot be read");
    }
    return store;
  }
  if (X509_STORE_load_file(store.get(), bundle.c_str()) != 1) {
    ERR_clear_error();
    throw std::runtime_error(bundle.string() + ": not a PEM certificate bundle");
  }
  if (sk_X509_OBJECT_num(X509_STORE_get0_objects(store.get())) <= 0) throw std::runtime_error(bundle.string() + ": holds no certificate");
  return store;
}

asio::io_context& context_of(const asio::any_io_executor& executor) {
  if (const auto* plain = executor.target<asio::io_context::executor_type>()) return plain->context();
  if (const auto* strand = executor.target<asio::strand<asio::io_context::executor_type>>()) return strand->get_inner_executor().context();
  throw std::invalid_argument("a stream connector runs on an io context's executor or a strand of one");
}

ListenerConfig listener_config(const ConnectorConfig& config) {
  ListenerConfig out;
  out.name = config.name;
  out.close_bound = config.close_bound;
  out.outbound_bound = config.outbound_bound;
  out.strand_per_stream = config.strand_per_stream;
  return out;
}

}  // namespace

// --- ClientTrust -------------------------------------------------------------------

ClientTrust::ClientTrust(std::filesystem::path bundle) : _bundle(std::move(bundle)) { reload(); }

ClientTrust::~ClientTrust() = default;

void ClientTrust::reload() {
  auto store = load_store(_bundle);
  contract::Lock lock(_mutex);
  _store = std::move(store);
  ++_generation;
}

uint64_t ClientTrust::generation() const {
  contract::Lock lock(_mutex);
  return _generation;
}

std::shared_ptr<X509_STORE> ClientTrust::store() const {
  contract::Lock lock(_mutex);
  return _store;
}

namespace detail {

std::shared_ptr<ServerTls> client_tls(const std::shared_ptr<X509_STORE>& store, TlsVersion minimum, const std::vector<std::string>& alpn,
                                      const Certificates::Material* material, const std::string& connector) {
  auto out = std::make_shared<ServerTls>(ssl::context::tls_client);
  SSL_CTX* native = out->context.native_handle();
  if (SSL_CTX_set_min_proto_version(native, minimum == TlsVersion::tls13 ? TLS1_3_VERSION : TLS1_2_VERSION) != 1)
    throw std::runtime_error(connector + ": cannot set the minimum TLS version");
  // The store is shared, read once (Lugh's review): the context takes a reference.
  if (!store) throw std::runtime_error(connector + ": cannot use its trust");
  SSL_CTX_set1_cert_store(native, store.get());
  SSL_CTX_set_verify(native, SSL_VERIFY_PEER, nullptr);
  if (material) {
    boost::system::error_code error;
    out->context.use_certificate_chain(asio::buffer(material->chain), error);
    if (!error) out->context.use_private_key(asio::buffer(material->key), ssl::context::pem, error);
    if (error) throw std::runtime_error(connector + ": cannot use its client certificate");
  }
  if (!alpn.empty()) {
    for (const auto& protocol : alpn) {
      if (protocol.empty() || protocol.size() > 255) throw std::runtime_error(connector + ": an ALPN protocol name is 1 to 255 bytes");
      out->alpn_wire.push_back(static_cast<char>(protocol.size()));
      out->alpn_wire += protocol;
    }
    if (SSL_CTX_set_alpn_protos(native, reinterpret_cast<const unsigned char*>(out->alpn_wire.data()), static_cast<unsigned>(out->alpn_wire.size())) != 0)
      throw std::runtime_error(connector + ": cannot offer its ALPN protocols");
  }
  return out;
}

}  // namespace detail

// --- StreamConnector ---------------------------------------------------------------

namespace detail {

// One connection being made: told once, whichever of its steps, its deadline or the
// connector's end comes first; a late step's result is dropped.
struct ConnectAttempt {
  ConnectAttempt(asio::any_io_executor on, asio::any_io_executor told, std::shared_ptr<ListenerShared> state, std::string to, uint16_t at,
                 StreamConnector::Connected tell)
      : executor(std::move(on)),
        reply(std::move(told)),
        shared(std::move(state)),
        host(std::move(to)),
        port(at),
        done(std::move(tell)),
        resolver(executor),
        socket(executor),
        timer(std::make_shared<asio::steady_timer>(executor)) {}

  asio::any_io_executor executor;
  asio::any_io_executor reply;
  std::shared_ptr<ListenerShared> shared;
  std::string host;
  uint16_t port;
  StreamConnector::Connected done;
  tcp::resolver resolver;
  tcp::socket socket;
  std::shared_ptr<asio::steady_timer> timer;
  std::atomic<bool> finished{false};
  // On the attempt's executor.
  const char* step = "resolve";
  std::shared_ptr<Stream> stream;

  std::string where() const { return host + ":" + std::to_string(port); }

  void count(uint64_t ListenerCounters::* counter) {
    contract::Lock lock(shared->mutex);
    ++(shared->counters.*counter);
  }

  void fail(std::string why) {
    if (finished.exchange(true)) return;
    timer->cancel();
    boost::system::error_code ignored;
    resolver.cancel();
    socket.close(ignored);
    if (stream) stream->abort();
    asio::post(reply, [done = std::move(done), why = std::move(why)] { done(nullptr, why); });
  }

  void succeed(std::shared_ptr<Stream> made) {
    if (finished.exchange(true)) return made->abort();
    // Counted when told, so an attempt is connected or failed, never both.
    count(&ListenerCounters::accepted);
    timer->cancel();
    asio::post(reply, [done = std::move(done), made = std::move(made)] { done(made, {}); });
  }
};

}  // namespace detail

using Attempt = detail::ConnectAttempt;

struct StreamConnector::Attempts {
  contract::Mutex mutex;
  std::vector<std::weak_ptr<Attempt>> pending DANNAN_GUARDED_BY(mutex);
};

// ListenerCounters keep the connector's counts, so no 0.8 type changes (the design's
// "How it stays a patch"): accepted is connected, refused_caller attempts,
// refused_cap resolution refused, refused_rate connection refused, refused_address
// timed out.
StreamConnector::StreamConnector(asio::io_context& io, ConnectorConfig config, const contract::TimeSource* time)
    : _config(std::move(config)), _shared(std::make_shared<ListenerShared>(io, listener_config(_config), time)), _attempts(std::make_shared<Attempts>()) {}

StreamConnector::StreamConnector(asio::any_io_executor executor, ConnectorConfig config, const contract::TimeSource* time)
    : StreamConnector(context_of(executor), std::move(config), time) {}

StreamConnector::~StreamConnector() {
  std::vector<std::shared_ptr<Attempt>> pending;
  {
    contract::Lock lock(_attempts->mutex);
    for (const auto& each : _attempts->pending)
      if (auto attempt = each.lock()) pending.push_back(std::move(attempt));
    _attempts->pending.clear();
  }
  for (auto& attempt : pending) asio::post(attempt->executor, [attempt] { attempt->fail(attempt->where() + ": the connector was closed"); });
  close_all();
}

void StreamConnector::connect(asio::any_io_executor executor, std::string host, uint16_t port, std::optional<ClientTls> tls, std::chrono::milliseconds deadline,
                              Connected done) {
  const auto on = _config.strand_per_stream ? asio::any_io_executor(asio::make_strand(executor)) : executor;
  auto attempt = std::make_shared<Attempt>(on, executor, _shared, std::move(host), port, std::move(done));
  attempt->count(&ListenerCounters::refused_caller);
  // What TLS needs is checked now, and its files were read when they were made: the
  // handshake reads none.
  std::shared_ptr<detail::ServerTls> context;
  std::string name;
  if (tls) {
    try {
      if (!tls->trust) throw std::invalid_argument(_config.name + ": TLS out needs its trust");
      const auto material = tls->certificate ? tls->certificate->current() : nullptr;
      context = detail::client_tls(tls->trust->store(), tls->minimum, tls->alpn, material.get(), _config.name);
      name = tls->server_name.empty() ? attempt->host : tls->server_name;
    } catch (const std::exception& error) {
      asio::post(attempt->executor, [attempt, why = std::string(error.what())] { attempt->fail(why); });
      return;
    }
  }
  {
    contract::Lock lock(_attempts->mutex);
    std::erase_if(_attempts->pending, [](const std::weak_ptr<Attempt>& each) { return each.expired(); });
    _attempts->pending.push_back(attempt);
  }
  const auto resolve = _config.resolve;
  // The time source outlives every attempt; held by pointer, not a reference to a local
  // (Lir's review).
  const contract::TimeSource* time = _shared->time;
  asio::post(attempt->executor, [attempt, context, name, resolve, deadline, time] {
    // One deadline over every step; a resolver that never answers is not waited for
    // (Lugh's review: getaddrinfo cannot be cancelled), and its late answer is dropped.
    // The deadline holds the attempt until it is told, whatever a step lets go of (a
    // resolver that drops its callback); telling it cancels the timer, which lets go.
    detail::when_time_reaches(attempt->timer, *time, time->now() + deadline, [attempt] {
      if (attempt->finished.load()) return;
      attempt->count(&ListenerCounters::refused_address);
      attempt->fail(attempt->where() + ": did not " + attempt->step + " within the deadline");
    });
    const auto resolved = [attempt, context, name](boost::system::error_code error, tcp::resolver::results_type addresses) {
      asio::post(attempt->executor, [attempt, context, name, error, addresses = std::move(addresses)] {
        if (attempt->finished.load()) return;
        if (error) {
          attempt->count(&ListenerCounters::refused_cap);
          return attempt->fail(attempt->where() + ": did not resolve: " + error.message());
        }
        attempt->step = "connect";
        asio::async_connect(attempt->socket, addresses, [attempt, context, name](boost::system::error_code connected, const tcp::endpoint&) {
          if (attempt->finished.load()) return;
          if (connected) {
            attempt->count(&ListenerCounters::refused_rate);
            return attempt->fail(attempt->where() + ": did not connect: " + connected.message());
          }
          boost::system::error_code ignored;
          attempt->socket.set_option(tcp::no_delay(true), ignored);
          auto stream = std::make_shared<Stream>(Stream::Private{}, std::move(attempt->socket), attempt->shared);
          {
            contract::Lock lock(attempt->shared->mutex);
            ++attempt->shared->counters.open;
            std::erase_if(attempt->shared->open, [](const std::weak_ptr<Stream>& each) { return each.expired(); });
            attempt->shared->open.push_back(stream);
          }
          if (!context) return attempt->succeed(std::move(stream));
          attempt->step = "complete the TLS handshake";
          attempt->stream = stream;
          stream->client_handshake(context, name, [attempt, stream](const std::string& why) {
            if (!why.empty()) return attempt->fail(attempt->where() + ": " + why);
            attempt->succeed(stream);
          });
        });
      });
    };
    if (resolve) {
      resolve(attempt->host, std::to_string(attempt->port), resolved);
    } else {
      attempt->resolver.async_resolve(attempt->host, std::to_string(attempt->port), resolved);
    }
  });
}

void StreamConnector::close_all() { close_all({}, {}); }

void StreamConnector::close_all(asio::any_io_executor executor, std::function<void()> done) {
  detail::close_open(*_shared, std::move(executor), std::move(done));
}

ConnectorCounters StreamConnector::counters() const {
  ListenerCounters kept;
  {
    contract::Lock lock(_shared->mutex);
    kept = _shared->counters;
  }
  ConnectorCounters out;
  out.attempts = kept.refused_caller;
  out.connected = kept.accepted;
  out.refused_resolve = kept.refused_cap;
  out.refused_connect = kept.refused_rate;
  out.timed_out = kept.refused_address;
  out.handshakes_failed = kept.handshakes_failed;
  out.closed_clean = kept.closed_clean;
  out.closed_abrupt = kept.closed_abrupt;
  out.slow_readers = kept.slow_readers;
  out.open = kept.open;
  return out;
}

}  // namespace dannan::listen
