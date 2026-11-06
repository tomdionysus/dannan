//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/server.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <atomic>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <cerrno>
#include <cstring>

namespace dannan::net {

namespace asio = boost::asio;

std::string congestion_control_unavailable(int error) {
  return std::string("peer sockets keep the system congestion control: bbr unavailable (") + std::strerror(error) + ")";
}

bool peer_socket_options(int fd, const status::ScopedLogger& log) {
  const int on = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
  ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
  const int idle = 60, interval = 15, count = 4;
#ifdef TCP_KEEPIDLE
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#else
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#endif
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
  ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#ifdef TCP_CONGESTION
  static constexpr char bbr[] = "bbr";
  if (::setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, bbr, sizeof(bbr) - 1) == 0) return true;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true)) log.warn(congestion_control_unavailable(errno));
  return false;
#else
  (void)log;
  return false;
#endif
}

// A connection between accept and authentication. Its socket and its
// timeout run on one strand, so the timer's close never races the reads.
struct Server::Handshaking {
  asio::strand<asio::any_io_executor> strand;
  Session::Socket socket;
  asio::steady_timer timer;
  Endpoint remote;

  // The family is read before the descriptor is released: arguments of one call have no
  // order, and g++ evaluates these right to left.
  Handshaking(asio::io_context& io, asio::ip::tcp::socket& tcp, Endpoint from)
      : Handshaking(io, tcp, std::move(from), tcp.local_endpoint().protocol().family()) {}
  Handshaking(asio::io_context& io, asio::ip::tcp::socket& tcp, Endpoint from, int family)
      : strand(asio::make_strand(io.get_executor())),
        socket(strand, Session::Socket::protocol_type(family, SOCK_STREAM), tcp.release()),
        timer(strand),
        remote(std::move(from)) {}

  // On the strand.
  void close() {
    boost::system::error_code ignored;
    socket.shutdown(asio::socket_base::shutdown_both, ignored);
    socket.close(ignored);
    timer.cancel();
  }
};

Server::Server(asio::io_context& io, std::string listen_host, uint16_t port, ServerOptions options, RequestHandler handler, Admitted admitted)
    : _io(io),
      _host(std::move(listen_host)),
      _port(port),
      _options(std::move(options)),
      _log(_options.log ? _options.log : status::null_sink(), "net"),
      _scheduler(
          _options.routes, _options.limits,
          [this, handler = std::move(handler)](const membership::NodeRecord& peer, sched::FrameType frame, const Message& request) {
            try {
              return handler(peer, frame, request);
            } catch (const std::exception& failure) {
              _log.debug(std::string("RPC handler: ") + failure.what());
              throw;
            }
          },
          _options.memory, _options.time, _options.threads, _options.log),
      _admitted(std::move(admitted)),
      // On a strand: its accept handler and stop()'s close never run at once.
      _acceptor(asio::make_strand(io)) {
  if (!_options.traffic) _options.traffic = std::make_shared<TransportTraffic>();
}

Server::~Server() { stop(); }

void Server::start() {
  const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(_host), _port);
  _acceptor.open(endpoint.protocol());
  _acceptor.set_option(asio::socket_base::reuse_address(true));
  _acceptor.bind(endpoint);
  _acceptor.listen(asio::socket_base::max_listen_connections);
  _bound_port = _acceptor.local_endpoint().port();
  accept();
}

void Server::begin_operation() {
  contract::Lock lock(_mutex);
  ++_operations;
}

void Server::end_operation() {
  // Under the lock: stop() may return and this server go the moment the count is seen
  // at zero, and the condition variable with it.
  contract::Lock lock(_mutex);
  --_operations;
  _idle.notify_all();
}

void Server::accept() {
  {
    contract::Lock lock(_mutex);
    if (_stopped) return;
    ++_operations;
  }
  _acceptor.async_accept(_io, [this](const boost::system::error_code& error, asio::ip::tcp::socket tcp) {
    if (!error) admit(std::move(tcp));
    if (error != asio::error::operation_aborted) accept();
    end_operation();
  });
}

void Server::admit(asio::ip::tcp::socket tcp) {
  boost::system::error_code ec;
  const auto remote = tcp.remote_endpoint(ec);
  if (ec) return;
  std::shared_ptr<Handshaking> connection;
  {
    contract::Lock lock(_mutex);
    // Past the bound the socket closes as it goes out of scope.
    if (_stopped || _handshaking.size() >= _options.max_pre_auth) return;
    peer_socket_options(tcp.native_handle(), _log);
    connection = std::make_shared<Handshaking>(_io, tcp, Endpoint{remote.address().to_string(), remote.port()});
    _handshaking.emplace(connection.get(), connection);
    _operations += 1;
  }
  asio::dispatch(connection->strand, [this, connection] {
    connection->timer.expires_after(_options.handshake_timeout);
    connection->timer.async_wait([connection](const boost::system::error_code& error) {
      if (!error) connection->close();
    });
    async_accept_handshake(connection->socket, _options.credentials, _options.local(), _options.max_frame, connection->remote,
                           [this, connection](std::exception_ptr error, Established session) { established(connection, error, std::move(session)); });
  });
}

void Server::established(const std::shared_ptr<Handshaking>& connection, std::exception_ptr error, Established session) {
  connection->timer.cancel();
  bool stopped;
  {
    contract::Lock lock(_mutex);
    _handshaking.erase(connection.get());
    stopped = _stopped;
  }
  // A probe proves the peer is reachable and authentic; it carries nothing.
  if (error || stopped || session.lane == Lane::probe) {
    if (error) {
      try {
        std::rethrow_exception(error);
      } catch (const std::exception& failure) {
        _log.debug(std::string("RPC session: ") + failure.what());
      } catch (...) {
      }
    } else if (!stopped) {
      _log.debug("node connection probe accepted peer=" + to_string(session.peer.id).substr(0, 12) + " remote=" + connection->remote.host);
    }
    connection->close();
    return end_operation();
  }
  const auto peer = session.peer;
  SessionHandlers handlers;
  handlers.request = [this, peer](Frame frame, Reply reply) { _scheduler.submit(peer, std::move(frame), std::move(reply)); };
  handlers.promoted = [this, id = peer.id](uint64_t request_id, sched::FrameType frame) { _scheduler.promote(id, request_id, frame); };
  handlers.cancelled = [this, id = peer.id](uint64_t request_id) { _scheduler.cancel(id, request_id); };
  auto key = std::make_shared<const Session*>(nullptr);
  handlers.closed = [this, key](std::string) {
    {
      contract::Lock lock(_mutex);
      _sessions.erase(*key);
    }
    end_operation();
  };
  SessionOptions options;
  options.memory = _options.memory;
  options.traffic = _options.traffic;
  options.time = _options.time;
  options.log = _options.log;
  auto established = Session::create(std::move(connection->socket), std::move(session), Role::acceptor, _options.routes, std::move(handlers), options);
  *key = established.get();
  {
    contract::Lock lock(_mutex);
    stopped = _stopped;
    if (!stopped) _sessions.emplace(established.get(), established);
  }
  // The handshake's count passes to the session; its close handler ends it.
  established->start();
  if (stopped) return established->close("server stopping");
  _log.info("node connection inbound peer=" + to_string(peer.id).substr(0, 12) + " lane=" + lane_name(established->lane()) +
            " remote=" + connection->remote.host + " advertised=" + peer.host + ":" + std::to_string(peer.port));
  if (_admitted) _admitted(established);
}

void Server::stop() {
  std::vector<std::shared_ptr<Session>> sessions;
  std::vector<std::shared_ptr<Handshaking>> handshaking;
  bool again;
  {
    contract::Lock lock(_mutex);
    again = _stopped;
    _stopped = true;
    for (auto& [_, session] : _sessions) sessions.push_back(session);
    for (auto& [_, connection] : _handshaking) handshaking.push_back(connection);
    // The acceptor's close below touches this object: counted, so the wait
    // covers it.
    ++_operations;
  }
  if (!again) {
    _log.debug("shutdown: RpcServer::stop begin");
    _log.debug("shutdown: RPC accept thread joining");
  }
  asio::post(_acceptor.get_executor(), [this] {
    boost::system::error_code ignored;
    _acceptor.close(ignored);
    end_operation();
  });
  for (auto& connection : handshaking) asio::post(connection->strand, [connection] { connection->close(); });
  if (!again) {
    _log.debug("shutdown: RPC accept thread joined");
    _log.debug("shutdown: RPC sessions reaping count=" + std::to_string(sessions.size()));
  }
  for (auto& session : sessions) session->close("server stopping");
  {
    contract::Lock lock(_mutex);
    _idle.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return _operations == 0; });
  }
  if (!again) _log.debug("shutdown: RPC sessions reaped");
  _scheduler.stop();
  if (!again) _log.debug("shutdown: RpcServer::stop complete");
}

size_t Server::sessions() const {
  contract::Lock lock(_mutex);
  return _sessions.size();
}

WorkStats Server::work_stats() const {
  auto stats = _scheduler.stats();
  stats.sessions = sessions();
  return stats;
}

size_t Server::pre_auth() const {
  contract::Lock lock(_mutex);
  return _handshaking.size();
}

}  // namespace dannan::net
