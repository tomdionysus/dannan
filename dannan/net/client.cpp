//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/client.h"

#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include "dannan/net/server.h"

namespace dannan::net {

namespace asio = boost::asio;

namespace {

std::string address_of(const Endpoint& endpoint) { return endpoint.host + ":" + std::to_string(endpoint.port); }

// Whether a call was stalled out, so its caller is told why: the session
// itself only knows it was cancelled.
struct Stall {
  std::atomic<bool> stalled{false};
  std::string why;  // written before `stalled` is set
};

// Watches one call for progress: every quarter of the deadline it asks the
// session how long the call has gone without a byte of its reply, and
// cancels it once that reaches the deadline. Touches only the session.
struct Watchdog : std::enable_shared_from_this<Watchdog> {
  asio::steady_timer timer;
  std::weak_ptr<Session> session;
  uint64_t request_id;
  std::chrono::milliseconds deadline;
  std::shared_ptr<Stall> stall;
  std::shared_ptr<std::atomic<bool>> finished;
  std::string why;

  Watchdog(asio::io_context& io, std::weak_ptr<Session> session, uint64_t request_id, std::chrono::milliseconds deadline, std::shared_ptr<Stall> stall,
           std::shared_ptr<std::atomic<bool>> finished, std::string why)
      : timer(io),
        session(std::move(session)),
        request_id(request_id),
        deadline(deadline),
        stall(std::move(stall)),
        finished(std::move(finished)),
        why(std::move(why)) {}

  void arm() {
    timer.expires_after(std::clamp(deadline / 4, std::chrono::milliseconds(5), std::chrono::milliseconds(250)));
    timer.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
      if (!error) self->check();
    });
  }

  void check() {
    if (finished->load()) return;
    const auto live = session.lock();
    if (!live) return;
    const auto idle = live->idle_for(request_id);
    if (!idle) return;
    if (*idle < deadline) return arm();
    stall->why = why;
    stall->stalled.store(true);
    live->cancel(request_id);
  }
};

}  // namespace

// One dial: resolve, connect, handshake, all on one strand so the timeout's
// close never races them.
struct Client::Dialling {
  asio::strand<asio::any_io_executor> strand;
  asio::ip::tcp::resolver resolver;
  asio::ip::tcp::socket tcp;
  std::optional<Session::Socket> socket;
  asio::steady_timer timer;
  // On the strand.
  bool timed_out = false;
  bool finished = false;

  explicit Dialling(asio::io_context& io) : strand(asio::make_strand(io.get_executor())), resolver(strand), tcp(strand), timer(strand) {}

  // On the strand.
  void abort() {
    boost::system::error_code ignored;
    resolver.cancel();
    tcp.close(ignored);
    if (socket) {
      socket->shutdown(Session::Socket::shutdown_both, ignored);
      socket->close(ignored);
    }
  }
};

Client::Client(asio::io_context& io, ClientOptions options) : _io(io), _options(std::move(options)), _self(_options.local().id) {
  if (!_options.traffic) _options.traffic = std::make_shared<TransportTraffic>();
}

Client::~Client() { stop(); }

Lane Client::lane_for(MessageType type) const {
  const auto* registration = _options.routes->find(type);
  return registration && registration->lanes == Lanes::data ? Lane::data : Lane::control;
}

void Client::call(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame, asio::any_io_executor executor, Done done) {
  call(Target{peer.id, {peer.host, peer.port}}, Call{type, std::move(payload), frame, std::move(executor), std::move(done)});
}

void Client::call(const Endpoint& endpoint, MessageType type, Bytes payload, sched::FrameType frame, asio::any_io_executor executor, Done done) {
  call(Target{std::nullopt, endpoint}, Call{type, std::move(payload), frame, std::move(executor), std::move(done)});
}

void Client::call(Target target, Call call) {
  const auto lane = lane_for(call.type);
  std::shared_ptr<Session> route;
  std::string key;
  bool first = false;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return fail(call, "client stopped");
    if (!target.id) {
      if (const auto known = _endpoint_ids.find(target.endpoint); known != _endpoint_ids.end()) target.id = known->second;
    }
    if (target.id) route = route_locked(*target.id, lane);
    if (route) {
      ++_stats.reused;
    } else {
      key = (target.id ? to_string(*target.id) : address_of(target.endpoint)) + "/" + lane_name(lane);
      auto& waiting = _dials[key];
      first = waiting.empty();
      waiting.push_back(std::move(call));
      if (first) ++_stats.dials;
    }
  }
  if (route) return place(route, std::move(call));
  if (first)
    dial(target, lane,
         [this, key, target, lane](std::shared_ptr<Session> session, std::string why) { dialled(key, target, lane, std::move(session), std::move(why)); });
}

void Client::fail(Call& call, std::string why) {
  asio::post(call.executor, [done = std::move(call.done), why = std::move(why)] { done(CallOutcome{std::nullopt, why}); });
}

void Client::place(const std::shared_ptr<Session>& session, Call call) {
  const auto lane = session->lane();
  const auto deadline = lane == Lane::data ? _options.data_no_progress : _options.control_no_progress;
  auto stall = std::make_shared<Stall>();
  auto finished = std::make_shared<std::atomic<bool>>(false);
  auto done = std::make_shared<Done>(std::move(call.done));
  const auto name = _options.routes->name(call.type);
  const auto admitted = session->call(call.type, std::move(call.payload), call.frame, call.executor, [stall, finished, done](CallOutcome outcome) {
    finished->store(true);
    if (!outcome.reply && stall->stalled.load()) outcome.failure = stall->why;
    (*done)(std::move(outcome));
  });
  if (!admitted.refused.empty()) {
    asio::post(call.executor, [done, why = admitted.refused] { (*done)(CallOutcome{std::nullopt, why}); });
    return;
  }
  if (deadline.count() <= 0) return;
  auto watchdog = std::make_shared<Watchdog>(
      _io, session, admitted.request_id, deadline, stall, finished,
      "no progress on " + name + " for " + std::to_string(deadline.count()) + " ms on the " + lane_name(lane) + " lane; cancelled for retry");
  watchdog->arm();
}

void Client::dial(Target target, Lane lane, std::function<void(std::shared_ptr<Session>, std::string)> finished) {
  auto attempt = std::make_shared<Dialling>(_io);
  bool stopped = false;
  {
    contract::Lock lock(_mutex);
    stopped = _stopped;
    if (!stopped) {
      _dialling.insert(attempt);
      ++_operations;
    }
  }
  if (stopped) return finished(nullptr, "client stopped");
  const auto address = address_of(target.endpoint);
  // Called once, on the strand.
  auto complete = [this, attempt, finished = std::move(finished)](std::shared_ptr<Session> session, std::string why) {
    if (attempt->finished) return;
    attempt->finished = true;
    attempt->timer.cancel();
    {
      contract::Lock lock(_mutex);
      _dialling.erase(attempt);
    }
    finished(std::move(session), std::move(why));
    end_operation();
  };
  auto failed = [attempt, complete, address](const std::string& stage, const std::string& error) {
    attempt->abort();
    complete(nullptr, "dial to " + address + " failed: " + (attempt->timed_out ? std::string("timed out") : stage + error));
  };
  asio::dispatch(attempt->strand, [this, attempt, target, lane, complete, failed] {
    attempt->timer.expires_after(_options.connect_timeout);
    attempt->timer.async_wait([attempt](const boost::system::error_code& error) {
      if (error || attempt->finished) return;
      attempt->timed_out = true;
      attempt->abort();
    });
    attempt->resolver.async_resolve(
        target.endpoint.host, std::to_string(target.endpoint.port),
        [this, attempt, target, lane, complete, failed](const boost::system::error_code& error, const asio::ip::tcp::resolver::results_type& results) {
          if (error) return failed("resolve: ", error.message());
          asio::async_connect(
              attempt->tcp, results,
              [this, attempt, target, lane, complete, failed](const boost::system::error_code& error, const asio::ip::tcp::endpoint& to) {
                if (error) return failed("", error.message());
                peer_socket_options(attempt->tcp.native_handle());
                attempt->socket.emplace(attempt->strand, Session::Socket::protocol_type(to.protocol().family(), SOCK_STREAM), attempt->tcp.release());
                async_dial_handshake(
                    *attempt->socket, _options.key, _options.local(), _options.max_frame, lane,
                    [this, attempt, target, lane, complete, failed](std::exception_ptr error, Established established) {
                      if (error) {
                        try {
                          std::rethrow_exception(error);
                        } catch (const std::exception& refused) {
                          return failed("handshake: ", refused.what());
                        }
                      }
                      if (target.id && established.peer.id != *target.id)
                        return failed("", "reached node " + to_string(established.peer.id) + ", not " + to_string(*target.id));
                      if (lane == Lane::probe) {
                        attempt->abort();
                        return complete(nullptr, "");
                      }
                      SessionHandlers handlers;
                      const auto& inbound = _options.inbound;
                      const auto peer = established.peer;
                      if (inbound.request)
                        handlers.request = [request = inbound.request, peer](Frame frame, Reply reply) { request(peer, std::move(frame), std::move(reply)); };
                      if (inbound.promoted)
                        handlers.promoted = [promoted = inbound.promoted, id = peer.id](uint64_t request_id, sched::FrameType frame) {
                          promoted(id, request_id, frame);
                        };
                      if (inbound.cancelled)
                        handlers.cancelled = [cancelled = inbound.cancelled, id = peer.id](uint64_t request_id) { cancelled(id, request_id); };
                      SessionOptions options;
                      options.memory = _options.memory;
                      options.traffic = _options.traffic;
                      options.time = _options.time;
                      auto session =
                          Session::create(std::move(*attempt->socket), std::move(established), Role::dialler, _options.routes, std::move(handlers), options);
                      attempt->socket.reset();
                      session->start();
                      complete(std::move(session), "");
                    });
              });
        });
  });
}

void Client::dialled(const std::string& key, const Target& target, Lane lane, std::shared_ptr<Session> session, std::string why) {
  std::vector<Call> waiting;
  Retire retire;
  std::shared_ptr<Session> route;
  bool stopped = false;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _dials.find(key); found != _dials.end()) {
      waiting = std::move(found->second);
      _dials.erase(found);
    }
    stopped = _stopped;
    if (!session) {
      ++_stats.dial_failures;
    } else if (!stopped) {
      const RouteKey route_key{session->peer().id, lane};
      _endpoint_ids[target.endpoint] = route_key.first;
      auto& slot = _outbound[route_key];
      if (slot && slot->usable() && slot != session) {
        retire.push_back(session);
        ++_stats.retired;
      } else {
        slot = session;
      }
      reconcile_locked(route_key, retire);
      route = route_locked(route_key.first, lane);
    }
  }
  if (session && stopped) {
    session->close("client stopped");
    why = "client stopped";
  }
  for (auto& loser : retire) loser->retire();
  for (auto& call : waiting) {
    if (route)
      place(route, std::move(call));
    else
      fail(call, why.empty() ? "no route to the peer after its dial" : why);
  }
}

void Client::admit(const std::shared_ptr<Session>& session) {
  if (session->lane() == Lane::probe) return;
  const auto& peer = session->peer();
  const RouteKey key{peer.id, session->lane()};
  Retire retire;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return;
    if (peer.port) _endpoint_ids[{peer.host, peer.port}] = peer.id;
    auto& slot = _inbound[key];
    if (slot && slot != session && slot->usable()) {
      // Two sessions from one peer on one lane: both ends keep the one with
      // the lower session id.
      if (session->established().keys.session_id < slot->established().keys.session_id) {
        retire.push_back(slot);
        slot = session;
      } else {
        retire.push_back(session);
      }
      ++_stats.retired;
    } else {
      slot = session;
    }
    reconcile_locked(key, retire);
  }
  for (auto& loser : retire) loser->retire();
}

void Client::reconcile_locked(const RouteKey& key, Retire& retire) {
  const auto outbound = _outbound.find(key);
  const auto inbound = _inbound.find(key);
  if (outbound == _outbound.end() || !outbound->second->usable()) return;
  if (inbound == _inbound.end() || !inbound->second->usable()) return;
  // Both ends dialled: both keep the session the lower node id dialled.
  if (_self < key.first) {
    retire.push_back(inbound->second);
    _inbound.erase(inbound);
  } else {
    retire.push_back(outbound->second);
    _outbound.erase(outbound);
  }
  ++_stats.retired;
}

std::shared_ptr<Session> Client::route_locked(const identity::NodeId& peer, Lane lane) {
  const RouteKey key{peer, lane};
  for (auto* routes : {&_outbound, &_inbound}) {
    const auto found = routes->find(key);
    if (found == routes->end()) continue;
    if (found->second->usable()) return found->second;
    routes->erase(found);
  }
  return nullptr;
}

void Client::probe(const Endpoint& endpoint, const identity::NodeId& expected, asio::any_io_executor executor, std::function<void(std::string)> done) {
  dial(Target{expected, endpoint}, Lane::probe,
       [executor, done = std::move(done)](std::shared_ptr<Session>, std::string why) { asio::post(executor, [done, why = std::move(why)] { done(why); }); });
}

bool Client::has_route(const identity::NodeId& peer, Lane lane) const {
  contract::Lock lock(_mutex);
  const RouteKey key{peer, lane};
  for (const auto* routes : {&_outbound, &_inbound}) {
    const auto found = routes->find(key);
    if (found != routes->end() && found->second->usable()) return true;
  }
  return false;
}

ClientStats Client::stats() const {
  contract::Lock lock(_mutex);
  return _stats;
}

void Client::end_operation() {
  {
    contract::Lock lock(_mutex);
    --_operations;
  }
  _idle.notify_all();
}

void Client::stop() {
  std::vector<std::shared_ptr<Session>> routes;
  std::vector<std::shared_ptr<Dialling>> dialling;
  std::map<std::string, std::vector<Call>> waiting;
  {
    contract::Lock lock(_mutex);
    _stopped = true;
    for (auto* table : {&_outbound, &_inbound}) {
      for (auto& [_, session] : *table) routes.push_back(session);
      table->clear();
    }
    dialling.assign(_dialling.begin(), _dialling.end());
    waiting.swap(_dials);
  }
  for (auto& session : routes) session->close("client stopped");
  for (auto& attempt : dialling) asio::post(attempt->strand, [attempt] { attempt->abort(); });
  for (auto& [_, calls] : waiting)
    for (auto& call : calls) fail(call, "client stopped");
  contract::Lock lock(_mutex);
  _idle.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return _operations == 0; });
}

}  // namespace dannan::net
