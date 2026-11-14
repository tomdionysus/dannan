//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dannan/contract/time_source.h"
#include "dannan/listen/certificates.h"
#include "dannan/listen/limits.h"
#include "dannan/listen/stream.h"

namespace dannan::listen {

struct ListenerConfig {
  // Names the listener in its counters and its streams.
  std::string name;
  std::string host = "0.0.0.0";
  // Zero: any; port() says which.
  uint16_t port = 0;
  // A listening socket handed in (inherited_socket()), adopted in place of binding;
  // the listener owns it from start().
  std::optional<int> inherited;
  // TLS: the material and the policy. None: plaintext.
  std::shared_ptr<Certificates> certificates;
  TlsPolicy tls;
  // With certificates: TLS comes later, when the hosted system calls start_tls(),
  // rather than at accept (StartTLS).
  bool start_tls = false;
  // The TLS handshake's deadline, at accept or at start_tls().
  std::chrono::milliseconds handshake_timeout{10000};
  // How long a clean close may take before the socket goes anyway (Lir's 2 s).
  std::chrono::milliseconds close_bound{2000};
  // Bytes send() may hold for a slow reader before the stream is closed; zero: no bound.
  size_t outbound_bound = 4u << 20;
  // Optional, off by default: connections open at once, and accepts a second.
  std::optional<size_t> max_connections;
  std::optional<Rate> accept_rate;
  // Shared with the hosted system's other listeners; none: no per-address or
  // per-caller limit.
  std::shared_ptr<Limits> limits;
  // True: each stream on its own strand of start()'s executor. False: on that
  // executor as it is given, for a hosted system that confines its connections to one
  // strand of its own (Lir's review); the executor must then serialise them itself.
  bool strand_per_stream = true;
};

struct ListenerCounters {
  uint64_t accepted = 0;
  uint64_t refused_cap = 0;
  uint64_t refused_rate = 0;
  uint64_t refused_address = 0;
  uint64_t refused_caller = 0;
  uint64_t handshakes_failed = 0;
  uint64_t closed_clean = 0;
  uint64_t closed_abrupt = 0;
  uint64_t slow_readers = 0;
  uint64_t open = 0;
};

// The public side's acceptor below the protocols (P3): accept, TLS or StartTLS,
// limits, deadlines on injected time, a bounded close, a stop in two steps and
// binding. Each connection served is handed up as a Stream on a strand of the
// executor given to start(). Parsing what flows over it is the hosted system's.
// Thread-safe.
class StreamListener {
 public:
  using Accepted = std::function<void(std::shared_ptr<Stream>)>;

  StreamListener(boost::asio::io_context& io, ListenerConfig config, const contract::TimeSource* time = &contract::steady_time_source());
  ~StreamListener();
  StreamListener(const StreamListener&) = delete;
  StreamListener& operator=(const StreamListener&) = delete;

  // Binds (or adopts the inherited socket) and accepts, each connection served handed
  // to `accepted` on its own strand of `executor`. Throws std::system_error when it
  // cannot bind, std::runtime_error for TLS it cannot set up.
  void start(boost::asio::any_io_executor executor, Accepted accepted);
  uint16_t port() const noexcept { return _port.load(); }

  // Stops accepting; what is open stays open.
  void stop_accepting();
  // Closes every connection it handed up, cleanly within the close bound.
  void close_all();
  // The same, and `done` posted to `executor` once every one of them has finished
  // (Lir's review: its stop waits for every client to go, bounded by the close bound).
  void close_all(boost::asio::any_io_executor executor, std::function<void()> done);
  // Both, with no window between: for a hosted system that tells its clients nothing.
  void stop();

  ListenerCounters counters() const;
  const ListenerConfig& config() const noexcept;

 private:
  // The accept loop holds only the shared state, never the listener, which may go first.
  static void accept(const std::shared_ptr<ListenerShared>& shared);
  static void served(const std::shared_ptr<ListenerShared>& shared, boost::asio::ip::tcp::socket socket);

  std::shared_ptr<ListenerShared> _shared;
  std::atomic<uint16_t> _port{0};
  std::atomic<bool> _accepting{false};
};

}  // namespace dannan::listen
