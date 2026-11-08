//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/net/scheduler.h"
#include "dannan/net/session.h"
#include "dannan/status/log.h"

namespace dannan::net {

// Keepalive at 60 s (then every 15 s, four probes), so a dead NAT mapping
// is noticed within two minutes; no Nagle; and BBR where the kernel offers
// it, so a bulk transfer keeps no standing queue for control frames to wait
// behind. Returns whether the socket got BBR. Every peer socket, dialled or
// accepted, gets these.
// BBR's absence is warned of once per process, through `log`, naming the
// reason. Where the platform has no choice of congestion control, nothing is.
bool peer_socket_options(int fd, const status::ScopedLogger& log);
std::string congestion_control_unavailable(int error);

struct ServerOptions {
  // How this node proves itself and checks its peers.
  Credentials credentials;
  // The node's current record, read at each handshake.
  std::function<membership::NodeRecord()> local;
  size_t max_frame = 256 * 1024;
  std::shared_ptr<const RouteTable> routes;
  ServerLimits limits;
  sched::RetainedMemoryLedger* memory = nullptr;
  std::shared_ptr<TransportTraffic> traffic;
  const contract::TimeSource* time = &contract::steady_time_source();
  // Where the server writes its journal lines; none, nowhere.
  std::shared_ptr<status::LogSink> log;
  // Where the workers' faults are recorded; none, nowhere readable.
  supervise::SupervisedThreads* threads = nullptr;
  // Connections accepted and not yet authenticated; past it a new one is
  // closed at once, so unauthenticated sockets cost bounded resources.
  size_t max_pre_auth = 8;
  // Called as each connection is admitted; a test's way to make admitting fail.
  std::function<void()> admit_hook;
  std::chrono::milliseconds handshake_timeout{5000};
};

// Accepts peers' connections, authenticates them, and serves their requests
// through the scheduler. An established session is offered to `admitted`
// (the client, so it can call back over it); a probe is closed once it has
// authenticated. Thread-safe.
class Server {
 public:
  using Admitted = std::function<void(const std::shared_ptr<Session>&)>;

  Server(boost::asio::io_context& io, std::string listen_host, uint16_t port, ServerOptions options, RequestHandler handler, Admitted admitted = {});
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds and starts accepting. Throws if the address cannot be bound.
  void start();
  // Stops accepting, closes every connection and session, waits for their
  // handlers, stops the scheduler. Must not run on the io context's only
  // thread.
  void stop();

  uint16_t bound_port() const noexcept { return _bound_port; }
  size_t sessions() const;
  size_t pre_auth() const;
  WorkStats work_stats() const;
  // Where the node's client sends requests that arrive on the sessions it
  // dialled, so every request is served by the same workers.
  RequestScheduler& scheduler() noexcept { return _scheduler; }
  const std::shared_ptr<TransportTraffic>& traffic() const noexcept { return _options.traffic; }

 private:
  struct Handshaking;

  void accept();
  void admit(boost::asio::ip::tcp::socket tcp);
  void established(const std::shared_ptr<Handshaking>& connection, std::exception_ptr error, Established session);
  // Every asynchronous operation that will touch this object is counted;
  // stop() waits for the count to reach zero.
  void begin_operation();
  void end_operation();

  boost::asio::io_context& _io;
  const std::string _host;
  const uint16_t _port;
  ServerOptions _options;
  const status::ScopedLogger _log;
  RequestScheduler _scheduler;
  Admitted _admitted;
  boost::asio::ip::tcp::acceptor _acceptor;
  boost::asio::steady_timer _accept_pause;
  uint16_t _bound_port = 0;

  mutable contract::Mutex _mutex;
  std::condition_variable _idle;
  std::map<const Session*, std::shared_ptr<Session>> _sessions DANNAN_GUARDED_BY(_mutex);
  std::map<const Handshaking*, std::shared_ptr<Handshaking>> _handshaking DANNAN_GUARDED_BY(_mutex);
  size_t _operations DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopped DANNAN_GUARDED_BY(_mutex) = false;
};

}  // namespace dannan::net
