//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <openssl/x509_vfy.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/listen/certificates.h"
#include "dannan/listen/stream.h"

// The outbound side of dannan::listen (TODO/2026-11-15-p3-stream-connector.md):
// connecting out over TCP or TLS, handing up the same Stream an accepted
// connection is, so a protocol's framing serves both ends. Asked by Lir (its
// client library), Lugh (its publisher and HTTPS exchange) and Mimir.
namespace dannan::listen {

struct ListenerShared;

// The CAs a connection's server is checked against, read once and kept in memory, so
// a bad bundle is found when this is made, not at the first connection, and no
// handshake reads a file (Lugh's review). Thread-safe.
class ClientTrust {
 public:
  // A PEM bundle of the CAs trusted; an empty path: the system's default store.
  // Throws std::runtime_error naming the path when it cannot be read or holds no
  // certificate. Waits on the device.
  explicit ClientTrust(std::filesystem::path bundle = {});
  ~ClientTrust();
  ClientTrust(const ClientTrust&) = delete;
  ClientTrust& operator=(const ClientTrust&) = delete;

  // Reads the bundle again. On failure keeps what it had and throws as the
  // constructor does. Connections already open keep what they began with.
  void reload();
  // Moves on at each successful load.
  uint64_t generation() const;
  const std::filesystem::path& bundle() const noexcept { return _bundle; }
  // The store as it is now, for a connection to verify against.
  std::shared_ptr<X509_STORE> store() const;

 private:
  const std::filesystem::path _bundle;
  mutable contract::Mutex _mutex;
  std::shared_ptr<X509_STORE> _store DANNAN_GUARDED_BY(_mutex);
  uint64_t _generation DANNAN_GUARDED_BY(_mutex) = 0;
};

// TLS out. The server's chain is checked against `trust` and its name against
// `server_name` (a DNS name with one wildcard label at most, never a partial one; an
// IP literal against the certificate's IP addresses, and then no SNI is sent, RFC 6066
// section 3). There is no option that turns verification off, and no OCSP or CRL
// check (Lugh's review).
struct ClientTls {
  // Required.
  std::shared_ptr<const ClientTrust> trust;
  // The name the server's certificate must carry, also sent as SNI; empty: the host
  // connected to.
  std::string server_name;
  // The client's own chain and key (CertificatePaths::client_ca unused), reloadable as
  // a listener's are; none: no client certificate is offered.
  std::shared_ptr<Certificates> certificate;
  TlsVersion minimum = TlsVersion::tls12;
  // Offered in order; the server's choice is Stream::alpn().
  std::vector<std::string> alpn;
};

struct ConnectorConfig {
  // Names the connector in its counters and its streams (Stream::listener()).
  std::string name;
  // How long a clean close may take before the socket goes anyway.
  std::chrono::milliseconds close_bound{2000};
  // Bytes send() may hold for a slow reader before the stream is closed; zero: no bound.
  size_t outbound_bound = 4u << 20;
  // True: each stream on its own strand of connect()'s executor. False: on that executor
  // as it is given, so a client passing its own strand shares it with its timers and
  // callbacks (Lir's review); the executor must then serialise the stream itself.
  bool strand_per_stream = true;
  // For tests: resolves in place of the system's resolver. Empty: the system's.
  std::function<void(const std::string& host, const std::string& port,
                     std::function<void(boost::system::error_code, boost::asio::ip::tcp::resolver::results_type)> done)>
      resolve;
};

struct ConnectorCounters {
  uint64_t attempts = 0;
  uint64_t connected = 0;
  uint64_t refused_resolve = 0;
  uint64_t refused_connect = 0;
  uint64_t handshakes_failed = 0;
  uint64_t timed_out = 0;
  uint64_t closed_clean = 0;
  uint64_t closed_abrupt = 0;
  uint64_t slow_readers = 0;
  uint64_t open = 0;
};

// Connects out. Connecting only: reconnect policy and backoff are the caller's protocol
// state. Thread-safe.
class StreamConnector {
 public:
  using Connected = std::function<void(std::shared_ptr<Stream>, std::string)>;

  StreamConnector(boost::asio::io_context& io, ConnectorConfig config, const contract::TimeSource* time = &contract::steady_time_source());
  // On an executor of an io context, or a strand of one (Lir's review); any other
  // executor throws std::invalid_argument.
  StreamConnector(boost::asio::any_io_executor executor, ConnectorConfig config, const contract::TimeSource* time = &contract::steady_time_source());
  // Closes what it made, and tells a connection still being made that it failed. Never
  // waits.
  ~StreamConnector();
  StreamConnector(const StreamConnector&) = delete;
  StreamConnector& operator=(const StreamConnector&) = delete;

  // Resolves `host`, connects to each address in turn, sets TCP_NODELAY, and with `tls`
  // takes the handshake, all within `deadline` on the injected time. `done` is posted to
  // `executor` once: the stream (on its own strand of `executor`, or on it as given), or
  // none and why, naming the host and the step, never a value read. From any thread.
  void connect(boost::asio::any_io_executor executor, std::string host, uint16_t port, std::optional<ClientTls> tls, std::chrono::milliseconds deadline,
               Connected done);

  // Closes every connection it made, cleanly within the close bound.
  void close_all();
  // The same, and `done` posted to `executor` once every one of them has finished.
  void close_all(boost::asio::any_io_executor executor, std::function<void()> done);

  ConnectorCounters counters() const;
  const ConnectorConfig& config() const noexcept { return _config; }

 private:
  struct Attempts;

  const ConnectorConfig _config;
  std::shared_ptr<ListenerShared> _shared;
  std::shared_ptr<Attempts> _attempts;
};

}  // namespace dannan::listen
