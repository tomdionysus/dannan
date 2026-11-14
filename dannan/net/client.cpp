//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/client.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <cmath>

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

// Watches one call for progress: every quarter of the deadline it asks how
// long the call has gone without a byte of its reply (nothing: no longer
// pending), and cancels it once that reaches the deadline. For a placed call
// both go to its session; for one a fault holds, to the client's hold.
struct Watchdog : std::enable_shared_from_this<Watchdog> {
  using Idle = std::function<std::optional<contract::Clock::duration>()>;
  asio::steady_timer timer;
  Idle idle;
  std::function<void()> cancel;
  std::chrono::milliseconds deadline;
  std::shared_ptr<Stall> stall;
  std::shared_ptr<std::atomic<bool>> finished;
  std::chrono::milliseconds notice;
  std::chrono::milliseconds next_notice;
  status::ScopedLogger log;
  std::string described;  // the call, as the journal names it
  const contract::TimeSource* time;
  contract::TimePoint placed;

  Watchdog(asio::io_context& io, Idle idle_of, std::function<void()> cancel_it, std::chrono::milliseconds limit, std::shared_ptr<Stall> stalls,
           std::shared_ptr<std::atomic<bool>> done, std::chrono::milliseconds every, status::ScopedLogger logger, std::string name,
           const contract::TimeSource* clock)
      : timer(io),
        idle(std::move(idle_of)),
        cancel(std::move(cancel_it)),
        deadline(limit),
        stall(std::move(stalls)),
        finished(std::move(done)),
        notice(every),
        next_notice(every),
        log(std::move(logger)),
        described(std::move(name)),
        time(clock),
        placed(clock->now()) {}

  std::string age() const { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(time->now() - placed).count()); }

  void arm() {
    const auto shortest = deadline.count() <= 0 ? notice : notice.count() <= 0 ? deadline : std::min(deadline, notice);
    timer.expires_after(std::clamp(shortest / 4, std::chrono::milliseconds(5), std::chrono::milliseconds(250)));
    timer.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
      if (!error) self->check();
    });
  }

  void check() {
    if (finished->load()) return;
    const auto since = idle();
    if (!since) return;
    const auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(*since);
    if (deadline.count() <= 0 || quiet < deadline) {
      // Progress starts the count again.
      if (notice.count() > 0 && quiet < notice) next_notice = notice;
      if (notice.count() > 0 && quiet >= next_notice) {
        log.debug("RPC stalled " + described + " age_ms=" + age() + " no_progress_ms=" + std::to_string(quiet.count()) +
                  (deadline.count() > 0 ? "; deadline_ms=" + std::to_string(deadline.count()) : "; request remains active while peer health is monitored"));
        next_notice = quiet + notice;
      }
      return arm();
    }
    // The caller is told what the journal says.
    stall->why = "RPC made no progress for " + std::to_string(quiet.count()) + " ms " + described + " age_ms=" + age() + "; cancelled for retry";
    log.debug(stall->why);
    stall->stalled.store(true);
    cancel();
  }
};

// A session's call, as a watchdog reads and cancels it.
Watchdog::Idle idle_on(std::weak_ptr<Session> session, uint64_t request_id) {
  return [session = std::move(session), request_id]() -> std::optional<contract::Clock::duration> {
    const auto live = session.lock();
    if (!live) return std::nullopt;
    return live->idle_for(request_id);
  };
}

std::string liveness_reason(const std::string& last_error) {
  return last_error.empty() ? std::string("health could not be established before dead_after") : "health unavailable until dead_after: " + last_error;
}

// What a timer should sleep for to look again at `at`, in `time`'s time.
std::chrono::milliseconds sleep_towards(const contract::TimeSource& time, contract::TimePoint at) {
  return time.wake_within(std::max(std::chrono::ceil<std::chrono::milliseconds>(at - time.now()), std::chrono::milliseconds(0)));
}

// How good a session's path is: direct (or reverse dialled) over punched
// over relayed.
int path_rank(Purpose purpose) {
  switch (purpose) {
    case Purpose::ordinary:
      return 2;
    case Purpose::punched:
      return 1;
    case Purpose::relayed:
      return 0;
  }
  return 0;
}

// Which of two sessions on one route both ends keep: the better path, then
// the lower session id. Both ends see the same purposes and ids, so they
// agree without a word.
bool preferred(const Session& a, const Session& b) {
  const auto ra = path_rank(a.established().purpose);
  const auto rb = path_rank(b.established().purpose);
  if (ra != rb) return ra > rb;
  return a.established().keys.session_id < b.established().keys.session_id;
}

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

Client::Client(asio::io_context& io, ClientOptions options)
    : _io(io),
      _options(std::move(options)),
      _log(_options.log ? _options.log : status::null_sink(), "net"),
      _self(_options.local().id),
      _health_strand(asio::make_strand(io)),
      _heartbeat(_health_strand) {
  if (!_options.traffic) _options.traffic = std::make_shared<TransportTraffic>();
}

Client::~Client() { stop(); }

Lane Client::lane_for(MessageType type) const {
  const auto* registration = _options.routes->find(type);
  return registration && registration->lanes == Lanes::data ? Lane::data : Lane::control;
}

CallHandle::CallHandle(const contract::TimeSource* time) : _time(time), _made(time->now()) {}

void CallHandle::placed(const std::shared_ptr<Session>& session, uint64_t request_id) {
  bool cancelled, aborted;
  std::optional<sched::FrameType> promoted;
  {
    contract::Lock lock(_mutex);
    _session = session;
    _id = request_id;
    _placed = _time->now();
    cancelled = _cancelled;
    aborted = _aborted;
    promoted = _promoted;
  }
  // Asked for before placement: applied now.
  if (promoted) session->promote(request_id, *promoted);
  if (cancelled) session->cancel(request_id);
  if (aborted) session->close("call aborted");
}

void CallHandle::finished() {
  contract::Lock lock(_mutex);
  _done = true;
  _complete_cancelled = {};
}

void CallHandle::on_cancel_before_placement(std::function<void()> complete) {
  contract::Lock lock(_mutex);
  _complete_cancelled = std::move(complete);
}

bool CallHandle::cancelled_before_placement() const {
  contract::Lock lock(_mutex);
  return _cancelled;
}

void CallHandle::cancel() {
  std::shared_ptr<Session> session;
  uint64_t id;
  std::function<void()> complete;
  {
    contract::Lock lock(_mutex);
    _cancelled = true;
    session = _session.lock();
    id = _id;
    // Not placed yet: complete it now rather than when its dial ends. The
    // call stays queued for its route and finds itself done when it gets there.
    if (!id && !_done) {
      complete = std::move(_complete_cancelled);
      _done = true;
    }
  }
  if (complete) complete();
  if (session && id) session->cancel(id);
}

void CallHandle::abort() {
  std::shared_ptr<Session> session;
  uint64_t id;
  std::function<void()> complete;
  {
    contract::Lock lock(_mutex);
    _cancelled = true;
    _aborted = true;
    session = _session.lock();
    id = _id;
    if (!id && !_done) {
      complete = std::move(_complete_cancelled);
      _done = true;
    }
  }
  if (complete) complete();
  if (!session || !id) return;
  session->cancel(id);
  session->close("call aborted");
}

void CallHandle::promote(sched::FrameType frame) {
  std::shared_ptr<Session> session;
  uint64_t id;
  {
    contract::Lock lock(_mutex);
    _promoted = frame;
    session = _session.lock();
    id = _id;
  }
  if (session && id) session->promote(id, frame);
}

std::chrono::milliseconds CallHandle::idle_for() const {
  std::shared_ptr<Session> session;
  uint64_t id;
  contract::TimePoint placed;
  {
    contract::Lock lock(_mutex);
    if (_done) return std::chrono::milliseconds::max();
    session = _session.lock();
    id = _id;
    placed = _placed;
  }
  const auto now = _time->now();
  const auto since_made = std::chrono::duration_cast<std::chrono::milliseconds>(now - _made);
  // Not placed yet: dialling or waiting for a route, nothing back so far.
  if (!id) return since_made;
  if (!session) return std::chrono::milliseconds::max();
  const auto idle = session->idle_for(id);
  if (!idle) return std::chrono::milliseconds::max();
  // No byte since it was placed: idle since it was made, the dial included.
  if (*idle >= now - placed) return since_made;
  return std::chrono::duration_cast<std::chrono::milliseconds>(*idle);
}

std::shared_ptr<CallHandle> Client::call(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame,
                                         asio::any_io_executor executor, Done done, CallWatch watch) {
  note_peer(peer);
  auto handle = std::shared_ptr<CallHandle>(new CallHandle(_options.time));
  auto completion = once(*handle, executor, std::move(done));
  call(Target{peer.id, {peer.host, peer.port}}, Call{type, std::move(payload), frame, std::move(executor), std::move(completion), 0, handle, watch});
  return handle;
}

std::shared_ptr<CallHandle> Client::call(const Endpoint& endpoint, MessageType type, Bytes payload, sched::FrameType frame, asio::any_io_executor executor,
                                         Done done, CallWatch watch) {
  auto handle = std::shared_ptr<CallHandle>(new CallHandle(_options.time));
  auto completion = once(*handle, executor, std::move(done));
  call(Target{std::nullopt, endpoint}, Call{type, std::move(payload), frame, std::move(executor), std::move(completion), 0, handle, watch});
  return handle;
}

Client::Done Client::once(CallHandle& handle, const asio::any_io_executor& executor, Done done) {
  // Completed once, by whichever comes first: the call's own end, or a cancel
  // of it before it is placed. Each runs on the call's executor.
  auto fired = std::make_shared<std::atomic<bool>>(false);
  auto shared = std::make_shared<Done>(std::move(done));
  handle.on_cancel_before_placement([fired, shared, executor] {
    asio::post(executor, [fired, shared] {
      if (!fired->exchange(true)) (*shared)(CallOutcome{std::nullopt, "RPC cancelled", {}});
    });
  });
  return [fired, shared](CallOutcome outcome) {
    if (!fired->exchange(true)) (*shared)(std::move(outcome));
  };
}

void Client::call(Target target, Call call) {
  if (_options.call_fault) {
    {
      contract::Lock lock(_mutex);
      if (!target.id)
        if (const auto known = _endpoint_ids.find(target.endpoint); known != _endpoint_ids.end()) target.id = known->second;
    }
    auto fault = _options.call_fault(target.id, target.endpoint, call.type);
    if (fault.kind == CallFault::Kind::refuse) return fail(call, std::move(fault.why));
    if (fault.kind == CallFault::Kind::stall) return stall(std::move(target), std::move(call));
  }
  const auto lane = lane_for(call.type);
  const auto backoff_key = address_of(target.endpoint) + "/" + lane_name(lane);
  std::shared_ptr<Session> route;
  std::shared_ptr<Session> ask_over;
  std::shared_ptr<asio::steady_timer> timer;
  bool path = false;
  std::string key;
  std::string refused;
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
    } else if (const auto backoff = _backoff.find(backoff_key); backoff != _backoff.end() && _options.time->now() < backoff->second.retry_after) {
      // Backoff governs new dials, never a route the peer opened.
      return fail(call, "peer " + address_of(target.endpoint) + " in retry backoff");
    } else if (target.id && !capable_locked(*target.id)) {
      // Never dialled: asked, over its own CONTROL session, to dial back.
      // Neither end dialable, or nothing to ask over: level 0 makes a path.
      const RouteKey wanted{*target.id, lane};
      ask_over = route_locked(*target.id, Lane::control);
      path = _options.make_path && (!ask_over || !local_capable());
      if (!ask_over && !path) {
        refused = "peer " + to_string(*target.id) + " accepts no inbound connections and is not connected";
      } else {
        auto [wait, fresh] = _reverse.try_emplace(wanted);
        wait->second.calls.push_back(std::move(call));
        if (fresh) {
          timer = std::make_shared<asio::steady_timer>(_health_strand);
          wait->second.timer = timer;
          wait->second.backoff_key = backoff_key;
          wait->second.path = path;
          ++_operations;
          if (path)
            ++_stats.paths_requested;
          else
            ++_stats.dial_requests_sent;
        }
      }
    } else {
      key = (target.id ? to_string(*target.id) : address_of(target.endpoint)) + "/" + lane_name(lane);
      auto [waiting, fresh] = _dials.try_emplace(key);
      waiting->second.push_back(std::move(call));
      first = fresh;
      if (first) ++_stats.dials;
    }
  }
  if (route) return place(route, std::move(call), target);
  if (!refused.empty()) {
    // The backoff a refused dial earns, so a retrying caller does not ask
    // over the wire on every attempt.
    observe_dial(backoff_key, false);
    return fail(call, refused);
  }
  if (timer && path) return ask_for_path({*target.id, lane}, timer);
  if (timer) return ask_for_lane({*target.id, lane}, ask_over, timer);
  if (first)
    dial(target, lane,
         [this, key, target, lane](std::shared_ptr<Session> session, std::string why) { dialled(key, target, lane, std::move(session), std::move(why)); });
}

void Client::await_peers_session(Target target, Lane lane, Call call) {
  const RouteKey wanted{*target.id, lane};
  std::shared_ptr<Session> route;
  std::shared_ptr<asio::steady_timer> timer;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return fail(call, "client stopped");
    route = route_locked(*target.id, lane);
    if (!route) {
      auto [wait, fresh] = _reverse.try_emplace(wanted);
      wait->second.calls.push_back(std::move(call));
      if (fresh) {
        timer = std::make_shared<asio::steady_timer>(_health_strand);
        wait->second.timer = timer;
        wait->second.peers_session = true;
        ++_operations;
      }
    }
  }
  if (route) return place(route, std::move(call), target);
  if (!timer) return;
  asio::post(_health_strand, [this, wanted, timer] {
    bool stopped;
    {
      contract::Lock lock(_mutex);
      stopped = _stopped;
      if (!stopped) _reverse_timers.insert(timer);
    }
    if (stopped) return end_operation();
    timer->expires_after(_options.connect_timeout);
    timer->async_wait([this, wanted, timer](const boost::system::error_code&) { reverse_expired(wanted, timer); });
  });
}

void Client::ask_for_lane(const RouteKey& key, const std::shared_ptr<Session>& control, const std::shared_ptr<asio::steady_timer>& timer) {
  // Armed before the request goes, so the route arriving can always cancel
  // it: the strand runs the two in order.
  asio::post(_health_strand, [this, key, timer] {
    bool stopped;
    {
      contract::Lock lock(_mutex);
      stopped = _stopped;
      if (!stopped) _reverse_timers.insert(timer);
    }
    if (stopped) return end_operation();
    timer->expires_after(_options.connect_timeout);
    timer->async_wait([this, key, timer](const boost::system::error_code&) { reverse_expired(key, timer); });
  });
  Writer writer;
  writer.u8(static_cast<uint8_t>(key.second));
  const Message request{message::dial_request, writer.take(), {}};
  bool sent = control->try_notify(request, sched::FrameType::control);
  if (!sent) {
    try {
      control->notify(request, sched::FrameType::control, _io.get_executor());
      sent = true;
    } catch (const OutboundRefused&) {
    }
  }
  if (sent) {
    _log.debug("dial request sent peer=" + to_string(key.first).substr(0, 12) + " lane=" + lane_name(key.second));
    return;
  }
  std::vector<Call> calls;
  std::string backoff_key;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _reverse.find(key); found != _reverse.end() && found->second.timer == timer) {
      calls = std::move(found->second.calls);
      backoff_key = found->second.backoff_key;
      _reverse.erase(found);
    }
  }
  asio::post(_health_strand, [timer] { timer->cancel(); });
  if (calls.empty()) return;
  observe_dial(backoff_key, false);
  for (auto& call : calls) fail(call, "peer " + to_string(key.first) + " accepts no inbound connections and the dial request was not queued");
}

void Client::reverse_expired(const RouteKey& key, const std::shared_ptr<asio::steady_timer>& timer) {
  std::vector<Call> calls;
  std::string backoff_key;
  bool path = false;
  bool peers_session = false;
  {
    contract::Lock lock(_mutex);
    _reverse_timers.erase(timer);
    if (const auto found = _reverse.find(key); found != _reverse.end() && found->second.timer == timer) {
      calls = std::move(found->second.calls);
      backoff_key = found->second.backoff_key;
      path = found->second.path;
      peers_session = found->second.peers_session;
      _reverse.erase(found);
    }
  }
  if (!calls.empty() && peers_session) {
    for (auto& call : calls)
      fail(call, "no route to peer " + to_string(key.first) + " on the " + lane_name(key.second) + " lane: its session did not arrive within " +
                     std::to_string(_options.connect_timeout.count()) + " ms of this end's being retired");
  } else if (!calls.empty()) {
    observe_dial(backoff_key, false);
    for (auto& call : calls)
      fail(call, path ? "no path to peer " + to_string(key.first) + " for the " + lane_name(key.second) + " lane within " +
                            std::to_string(_options.path_timeout.count()) + " ms"
                      : "peer " + to_string(key.first) + " accepts no inbound connections; its reverse dial for the " + lane_name(key.second) +
                            " lane did not arrive within " + std::to_string(_options.connect_timeout.count()) + " ms");
  }
  end_operation();
}

void Client::ask_for_path(const RouteKey& key, const std::shared_ptr<asio::steady_timer>& timer) {
  asio::post(_health_strand, [this, key, timer] {
    bool stopped;
    {
      contract::Lock lock(_mutex);
      stopped = _stopped;
      if (!stopped) _reverse_timers.insert(timer);
    }
    if (stopped) return end_operation();
    timer->expires_after(_options.path_timeout);
    timer->async_wait([this, key, timer](const boost::system::error_code&) { reverse_expired(key, timer); });
  });
  _options.make_path(key.first, key.second, [this, key, timer](std::string why) {
    abandon_wait(key, timer, "no path to peer " + to_string(key.first) + " for the " + lane_name(key.second) + " lane: " + why);
  });
}

void Client::abandon_wait(const RouteKey& key, const std::shared_ptr<asio::steady_timer>& timer, const std::string& why) {
  std::vector<Call> calls;
  std::string backoff_key;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _reverse.find(key); found != _reverse.end() && found->second.timer == timer) {
      calls = std::move(found->second.calls);
      backoff_key = found->second.backoff_key;
      _reverse.erase(found);
    }
  }
  asio::post(_health_strand, [timer] { timer->cancel(); });
  if (calls.empty()) return;
  observe_dial(backoff_key, false);
  for (auto& call : calls) fail(call, why);
}

bool Client::capable_locked(const identity::NodeId& peer) const {
  const auto found = _peer_capabilities.find(peer);
  return found == _peer_capabilities.end() || (found->second & membership::capability_inbound) != 0;
}

bool Client::local_capable() const { return _options.local().inbound_capable(); }

void Client::note_peer(const membership::NodeRecord& peer) {
  if (peer.id == identity::NodeId{}) return;
  contract::Lock lock(_mutex);
  _peer_capabilities[peer.id] = peer.capabilities;
}

void Client::set_maintained_peers(std::function<std::vector<membership::NodeRecord>()> peers) {
  contract::Lock lock(_mutex);
  _maintained = std::move(peers);
}

void Client::request_lane(const membership::NodeRecord& peer, Lane lane) {
  if (peer.id == identity::NodeId{} || peer.host.empty() || !peer.port) return;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return;
    _peer_capabilities[peer.id] = peer.capabilities;
    _requested[{peer.id, lane}] = peer;
    ++_stats.dial_requests_received;
    ++_operations;
  }
  asio::post(_health_strand, [this] {
    open_lanes();
    end_operation();
  });
}

Handled Client::handle_dial_request(const membership::NodeRecord& peer, const Message& request) {
  try {
    Reader reader(request.payload);
    const auto lane = static_cast<Lane>(reader.u8());
    reader.finish();
    if (lane != Lane::control && lane != Lane::data) return Handled::failed(400, "invalid_request", "invalid transport lane");
    _log.debug("dial request received peer=" + to_string(peer.id).substr(0, 12) + " lane=" + lane_name(lane));
    request_lane(peer, lane);
    return Handled::processed();
  } catch (const DecodeError& error) {
    return Handled::failed(400, "invalid_request", error.what());
  }
}

void Client::open_lanes() {
  std::vector<std::pair<membership::NodeRecord, Lane>> wanted;
  std::function<std::vector<membership::NodeRecord>()> maintained;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return;
    for (auto& [key, peer] : _requested) wanted.emplace_back(peer, key.second);
    _requested.clear();
    maintained = _maintained;
  }
  // An incapable node keeps both lanes open to every capable peer itself;
  // dial_request only covers the gap until the next pass.
  if (maintained && !local_capable()) {
    for (const auto& peer : maintained()) {
      wanted.emplace_back(peer, Lane::control);
      wanted.emplace_back(peer, Lane::data);
    }
  }
  for (const auto& [peer, lane] : wanted) {
    // Two incapable nodes have no path to each other.
    if (peer.inbound_capable()) ensure_lane(peer, lane);
  }
}

void Client::ensure_lane(const membership::NodeRecord& peer, Lane lane) {
  const Target target{peer.id, {peer.host, peer.port}};
  const auto key = to_string(peer.id) + "/" + lane_name(lane);
  {
    contract::Lock lock(_mutex);
    if (_stopped || route_locked(peer.id, lane)) return;
    if (const auto backoff = _backoff.find(address_of(target.endpoint) + "/" + lane_name(lane));
        backoff != _backoff.end() && _options.time->now() < backoff->second.retry_after)
      return;
    if (!_dials.try_emplace(key).second) return;
    ++_stats.dials;
  }
  dial(target, lane, [this, key, target, lane](std::shared_ptr<Session> session, std::string why) {
    // Expected while a peer is down; the backoff prevents a dial loop.
    if (!session) _log.debug("lane maintenance peer=" + to_string(*target.id).substr(0, 12) + " lane=" + lane_name(lane) + ": " + why);
    dialled(key, target, lane, std::move(session), std::move(why));
  });
}

void Client::invalidate_identity_association(const membership::IdentityReset& reset) {
  if (reset.host.empty() || !reset.epoch) return;
  std::vector<std::shared_ptr<Session>> routes;
  {
    contract::Lock lock(_mutex);
    std::set<identity::NodeId> stale;
    for (auto it = _endpoint_ids.begin(); it != _endpoint_ids.end();) {
      if (reset.matches_endpoint(it->first.host, it->first.port) && reset.matches_node(it->second)) {
        stale.insert(it->second);
        _backoff.erase(address_of(it->first) + "/" + lane_name(Lane::control));
        _backoff.erase(address_of(it->first) + "/" + lane_name(Lane::data));
        it = _endpoint_ids.erase(it);
      } else {
        ++it;
      }
    }
    for (auto* table : {&_outbound, &_inbound}) {
      for (auto it = table->begin(); it != table->end();) {
        const auto& peer = it->second->peer();
        if (stale.contains(it->first.first) || (reset.matches_endpoint(peer.host, peer.port) && reset.matches_node(peer.id))) {
          routes.push_back(it->second);
          it = table->erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  for (auto& session : routes) session->close("identity association reset");
}

void Client::close_route(const identity::NodeId& peer, Lane lane, const std::string& why) {
  std::vector<std::shared_ptr<Session>> routes;
  {
    contract::Lock lock(_mutex);
    for (auto* table : {&_outbound, &_inbound}) {
      if (const auto found = table->find({peer, lane}); found != table->end()) {
        routes.push_back(found->second);
        table->erase(found);
      }
    }
  }
  for (auto& session : routes) session->close(why);
}

void Client::fail(Call& call, std::string why) {
  if (call.handle) call.handle->finished();
  asio::post(call.executor, [done = std::move(call.done), why = std::move(why)] { done(CallOutcome{std::nullopt, why, {}}); });
}

void Client::place(const std::shared_ptr<Session>& session, Call call, const Target& target) {
  if (call.handle && call.handle->cancelled_before_placement()) return fail(call, "RPC cancelled");
  const auto lane = session->lane();
  const auto [deadline, notice] = watch_of(call, lane);
  const auto frame = call.frame;
  auto stall = std::make_shared<Stall>();
  auto finished = std::make_shared<std::atomic<bool>>(false);
  auto done = std::make_shared<Done>(std::move(call.done));
  const auto name = _options.routes->name(call.type);
  auto handle = call.handle;
  // Moved from only once admitted: a refusal by a retiring session leaves it
  // to be routed again.
  if (_options.place_hook) _options.place_hook(*session);
  const auto admitted = session->call(call.type, std::move(call.payload), call.frame, call.executor, [stall, finished, done, handle](CallOutcome outcome) {
    finished->store(true);
    if (handle) handle->finished();
    if (!outcome.reply && stall->stalled.load()) outcome.failure = stall->why;
    (*done)(std::move(outcome));
  });
  if (!admitted.refused.empty()) {
    const bool gone = admitted.refused == "peer channel is retiring" || admitted.refused == "peer channel is closed";
    if (gone && call.reroutes < 3) {
      call.done = std::move(*done);
      ++call.reroutes;
      // Simultaneous dials keep one session a lane, so this end's can go before the
      // peer's registers here: wait for that one rather than dial again.
      auto known = target;
      known.id = session->peer().id;
      return await_peers_session(std::move(known), lane, std::move(call));
    }
    if (handle) handle->finished();
    asio::post(call.executor, [done, why = admitted.refused] { (*done)(CallOutcome{std::nullopt, why, {}}); });
    return;
  }
  if (handle) handle->placed(session, admitted.request_id);
  if (deadline.count() <= 0 && notice.count() <= 0) return;
  const std::weak_ptr<Session> watched = session;
  const auto request_id = admitted.request_id;
  auto watchdog = std::make_shared<Watchdog>(
      _io, idle_on(watched, request_id),
      [watched, request_id] {
        if (const auto live = watched.lock()) live->cancel(request_id);
      },
      deadline, stall, finished, notice, _log,
      std::string("(") + sched::frame_type_name(frame) + ") peer=" + to_string(session->peer().id).substr(0, 12) + " message=" + name, _options.time);
  watchdog->arm();
}

std::pair<std::chrono::milliseconds, std::chrono::milliseconds> Client::watch_of(const Call& call, Lane lane) const {
  const auto* rules = _options.routes->find(call.type);
  const auto deadline = call.watch.no_progress        ? *call.watch.no_progress
                        : rules && rules->no_progress ? *rules->no_progress
                        : lane == Lane::data          ? _options.data_no_progress
                                                      : _options.control_no_progress;
  const auto notice = call.watch.stall_notice        ? *call.watch.stall_notice
                      : rules && rules->stall_notice ? *rules->stall_notice
                      : lane == Lane::data           ? _options.data_stall_notice
                                                     : _options.control_stall_notice;
  return {deadline, notice};
}

void Client::stall(Target target, Call call) {
  const auto lane = lane_for(call.type);
  const auto [deadline, notice] = watch_of(call, lane);
  const auto handle = call.handle;
  const auto described = std::string("(") + sched::frame_type_name(call.frame) +
                         ") peer=" + (target.id ? to_string(*target.id).substr(0, 12) : address_of(target.endpoint)) +
                         " message=" + _options.routes->name(call.type) + " stalled by the test";
  auto held = std::make_shared<Stalled>();
  held->peer = target.id;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return fail(call, "client stopped");
    {
      contract::Lock hold(held->mutex);
      held->call = std::move(call);
    }
    _stalled.emplace(_next_stalled++, held);
  }
  if (deadline.count() <= 0 && notice.count() <= 0) return;
  // Watched as a placed call is: idle since it was made, since nothing comes.
  // The watchdog holds only the held call, never the client, which may go first.
  auto stalled_out = std::make_shared<Stall>();
  auto finished = std::make_shared<std::atomic<bool>>(false);
  const std::weak_ptr<Stalled> weak = held;
  auto watchdog = std::make_shared<Watchdog>(
      _io,
      [weak, handle]() -> std::optional<contract::Clock::duration> {
        const auto live = weak.lock();
        if (!live || !live->pending() || handle->cancelled_before_placement()) return std::nullopt;
        return handle->idle_for();
      },
      [weak, stalled_out] {
        if (const auto live = weak.lock())
          if (auto taken = live->take()) fail(*taken, stalled_out->why);
      },
      deadline, stalled_out, finished, notice, _log, described, _options.time);
  watchdog->arm();
}

bool Client::Stalled::pending() const {
  contract::Lock lock(mutex);
  return call.has_value();
}

std::optional<Client::Call> Client::Stalled::take() {
  contract::Lock lock(mutex);
  auto out = std::move(call);
  call.reset();
  return out;
}

size_t Client::release_stalled(const std::optional<identity::NodeId>& peer) {
  std::vector<std::shared_ptr<Stalled>> released;
  {
    contract::Lock lock(_mutex);
    for (auto it = _stalled.begin(); it != _stalled.end();) {
      if (peer && it->second->peer != peer) {
        ++it;
        continue;
      }
      released.push_back(std::move(it->second));
      it = _stalled.erase(it);
    }
  }
  size_t failed = 0;
  for (const auto& held : released) {
    auto call = held->take();
    if (!call) continue;
    if (!call->handle->cancelled_before_placement()) ++failed;
    fail(*call, "stall released");
  }
  return failed;
}

size_t Client::stalled() const {
  std::vector<std::shared_ptr<Stalled>> held;
  {
    contract::Lock lock(_mutex);
    for (const auto& [_, each] : _stalled) held.push_back(each);
  }
  size_t out = 0;
  for (const auto& each : held) {
    contract::Lock lock(each->mutex);
    if (each->call && !each->call->handle->cancelled_before_placement()) ++out;
  }
  return out;
}

namespace {

// Ends an operation however its holder leaves, a throw included: one left open
// would hold stop() for ever. Armed after the count is taken.
class EndsOperation {
 public:
  EndsOperation() = default;
  explicit EndsOperation(std::function<void()> end) : _end(std::move(end)) {}
  EndsOperation(const EndsOperation&) = delete;
  EndsOperation& operator=(const EndsOperation&) = delete;
  ~EndsOperation() {
    if (_end) _end();
  }
  void arm(std::function<void()> end) { _end = std::move(end); }
  void release() { _end = {}; }

 private:
  std::function<void()> _end;
};

}  // namespace

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
  if (_options.dial_hook) {
    EndsOperation held([this] { end_operation(); });
    if (const auto refused = _options.dial_hook(target.endpoint, lane); !refused.empty()) {
      {
        contract::Lock lock(_mutex);
        _dialling.erase(attempt);
      }
      return finished(nullptr, "dial to " + address + " failed: " + refused);
    }
    // Going on: complete() ends it.
    held.release();
  }
  // Called once, on the strand.
  auto complete = [this, attempt, finished = std::move(finished)](std::shared_ptr<Session> session, std::string why) {
    if (attempt->finished) return;
    attempt->finished = true;
    const EndsOperation held([this] { end_operation(); });
    attempt->timer.cancel();
    {
      contract::Lock lock(_mutex);
      _dialling.erase(attempt);
    }
    finished(std::move(session), std::move(why));
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
              [this, attempt, target, lane, complete, failed](const boost::system::error_code& connect_error, const asio::ip::tcp::endpoint& to) {
                if (connect_error) return failed("", connect_error.message());
                peer_socket_options(attempt->tcp.native_handle(), _log);
                attempt->socket.emplace(attempt->strand, Session::Socket::protocol_type(to.protocol().family(), SOCK_STREAM), attempt->tcp.release());
                async_dial_handshake(*attempt->socket, _options.credentials, _options.local(), _options.max_frame, lane,
                                     [this, attempt, target, lane, complete, failed](std::exception_ptr refusal, Established established) {
                                       if (refusal) {
                                         try {
                                           std::rethrow_exception(refusal);
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
                                       auto handlers = handlers_for(established.peer);
                                       auto session = Session::create(std::move(*attempt->socket), std::move(established), Role::dialler, _options.routes,
                                                                      std::move(handlers), session_options());
                                       attempt->socket.reset();
                                       // Known before its first request can be read.
                                       if (_options.established) _options.established(session->established());
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
  std::shared_ptr<asio::steady_timer> settled_wait;
  bool stopped = false;
  {
    // One section from reading _stopped to taking the slot: a stop() between the two
    // would leave this session a route after stop() emptied the tables, unclosed.
    contract::Lock lock(_mutex);
    if (const auto found = _dials.find(key); found != _dials.end()) {
      waiting = std::move(found->second);
      _dials.erase(found);
    }
    stopped = _stopped;
    if (!session) ++_stats.dial_failures;
    observe_dial_locked(address_of(target.endpoint) + "/" + lane_name(lane), session != nullptr);
    if (session && !stopped) {
      ++_stats.connections_created;
      const RouteKey route_key{session->peer().id, lane};
      _endpoint_ids[target.endpoint] = route_key.first;
      _peer_capabilities[route_key.first] = session->peer().capabilities;
      auto& slot = _outbound[route_key];
      if (slot && slot->usable() && slot != session) {
        // Two of this end's dials to one peer and lane: both ends keep the
        // one with the better path, then the lower session id.
        if (preferred(*session, *slot)) {
          retire.push_back(slot);
          slot = session;
          hold_locked(session);
        } else {
          retire.push_back(session);
        }
        ++_stats.retired;
      } else {
        slot = session;
        hold_locked(session);
      }
      reconcile_locked(route_key, retire);
      route = route_locked(route_key.first, lane);
      // Calls waiting for a route to this peer have one now.
      if (const auto found = _reverse.find(route_key); route && found != _reverse.end() && found->second.peers_session) {
        for (auto& call : found->second.calls) waiting.push_back(std::move(call));
        settled_wait = found->second.timer;
        _reverse.erase(found);
      }
    }
  }
  if (settled_wait) asio::post(_health_strand, [settled_wait] { settled_wait->cancel(); });
  if (session && stopped) {
    session->close("client stopped");
    why = "client stopped";
  }
  retire_sessions(retire);
  if (session && !stopped && route == session)
    _log.info("node connection outbound peer=" + to_string(session->peer().id).substr(0, 12) + " lane=" + lane_name(lane) +
              " endpoint=" + address_of(target.endpoint));
  for (auto& call : waiting) {
    if (route)
      place(route, std::move(call), target);
    else
      fail(call, why.empty() ? "no route to the peer after its dial" : why);
  }
}

void Client::admit(const std::shared_ptr<Session>& session) {
  if (session->lane() == Lane::probe) return;
  const auto& peer = session->peer();
  const RouteKey key{peer.id, session->lane()};
  Retire retire;
  std::vector<Call> waiting;
  std::shared_ptr<asio::steady_timer> timer;
  std::shared_ptr<Session> route;
  // Declared before the lock, so it ends the operation after the lock is let go.
  EndsOperation held;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return;
    // Held across the table change and its retirements, so stop() waits for
    // the losers this admission takes off the tables.
    ++_operations;
    held.arm([this] { end_operation(); });
    if (peer.port) _endpoint_ids[{peer.host, peer.port}] = peer.id;
    _peer_capabilities[peer.id] = peer.capabilities;
    auto& slot = _inbound[key];
    if (slot && slot != session && slot->usable()) {
      // Two sessions from one peer on one lane: both ends keep the one with
      // the better path, then the lower session id.
      if (preferred(*session, *slot)) {
        retire.push_back(slot);
        slot = session;
        hold_locked(session);
      } else {
        retire.push_back(session);
      }
      ++_stats.retired;
    } else {
      slot = session;
      hold_locked(session);
    }
    reconcile_locked(key, retire);
    // A reverse dial some calls were waiting for.
    if (const auto found = _reverse.find(key); found != _reverse.end()) {
      route = route_locked(key.first, key.second);
      if (route) {
        waiting = std::move(found->second.calls);
        timer = found->second.timer;
        _reverse.erase(found);
      }
    }
  }
  retire_sessions(retire);
  if (timer) {
    _log.debug("dial request answered peer=" + to_string(key.first).substr(0, 12) + " lane=" + lane_name(key.second));
    asio::post(_health_strand, [timer] { timer->cancel(); });
  }
  for (auto& call : waiting) place(route, std::move(call), Target{key.first, {peer.host, peer.port}});
}

// Every caller holds an operation from its table change to here, so a stop()
// that began between the two waits for the closes below.
void Client::retire_sessions(Retire& retire) {
  if (retire.empty()) return;
  bool stopped;
  {
    // Remembered until they close: a retiring session may still hold calls
    // that stop() must fail. A loser taken off the tables before stop() and
    // reaching here after it is in neither, so it is closed here instead, and
    // waited for as stop() waits for its own.
    contract::Lock lock(_mutex);
    stopped = _stopped;
    if (stopped) {
      _operations += retire.size();
    } else {
      for (const auto& session : retire) hold_locked(session);
    }
  }
  for (auto& loser : retire) {
    if (stopped) {
      loser->close("client stopped");
      loser->after_queued([this] { end_operation(); });
    } else {
      loser->retire();
    }
  }
}

void Client::reconcile_locked(const RouteKey& key, Retire& retire) {
  const auto outbound = _outbound.find(key);
  const auto inbound = _inbound.find(key);
  if (outbound == _outbound.end() || !outbound->second->usable()) return;
  if (inbound == _inbound.end() || !inbound->second->usable()) return;
  // A better path wins whoever dialled it: a punch replacing a relay.
  const auto out_rank = path_rank(outbound->second->established().purpose);
  const auto in_rank = path_rank(inbound->second->established().purpose);
  if (out_rank != in_rank) {
    if (out_rank > in_rank) {
      retire.push_back(inbound->second);
      _inbound.erase(inbound);
    } else {
      retire.push_back(outbound->second);
      _outbound.erase(outbound);
    }
    ++_stats.retired;
    return;
  }
  // Both ends dialled: both keep the session the lower node id dialled. An
  // incapable node keeps its own when the peer is capable: nothing can
  // replace it from the other side. Two incapable nodes, whose sessions
  // are level 0's, keep the lower id's like any pair.
  if (_self < key.first || (!local_capable() && capable_locked(key.first))) {
    retire.push_back(inbound->second);
    _inbound.erase(inbound);
  } else {
    retire.push_back(outbound->second);
    _outbound.erase(outbound);
  }
  ++_stats.retired;
}

void Client::hold_locked(const std::shared_ptr<Session>& session) {
  std::erase_if(_held, [&](const std::weak_ptr<Session>& weak) {
    const auto held = weak.lock();
    return !held || held == session;
  });
  _held.push_back(session);
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

SessionHandlers Client::handlers_for(const membership::NodeRecord& peer) const {
  SessionHandlers handlers;
  const auto& inbound = _options.inbound;
  if (inbound.request) handlers.request = [request = inbound.request, peer](Frame frame, Reply reply) { request(peer, std::move(frame), std::move(reply)); };
  if (inbound.promoted)
    handlers.promoted = [promoted = inbound.promoted, id = peer.id](uint64_t request_id, sched::FrameType frame) { promoted(id, request_id, frame); };
  if (inbound.cancelled) handlers.cancelled = [cancelled = inbound.cancelled, id = peer.id](uint64_t request_id) { cancelled(id, request_id); };
  return handlers;
}

SessionOptions Client::session_options() const {
  SessionOptions options;
  options.memory = _options.memory;
  options.traffic = _options.traffic;
  options.time = _options.time;
  options.log = _options.log;
  return options;
}

void Client::adopt(int descriptor, int family, const identity::NodeId& peer, Lane lane, Role role, Purpose purpose, PathToken token,
                   std::function<void(std::string)> done) {
  auto attempt = std::make_shared<Dialling>(_io);
  bool stopped;
  {
    contract::Lock lock(_mutex);
    stopped = _stopped;
    if (!stopped) {
      _dialling.insert(attempt);
      ++_operations;
    }
  }
  if (stopped) {
    ::close(descriptor);
    if (done) asio::post(_io, [done] { done("client stopped"); });
    return;
  }
  // Called once, on the strand.
  auto finish = [this, attempt, done = std::move(done)](std::string why) {
    if (attempt->finished) return;
    attempt->finished = true;
    const EndsOperation held([this] { end_operation(); });
    attempt->timer.cancel();
    {
      contract::Lock lock(_mutex);
      _dialling.erase(attempt);
      if (why.empty()) ++_stats.paths_adopted;
    }
    if (done) asio::post(_io, [done, why = std::move(why)] { done(why); });
  };
  asio::dispatch(attempt->strand, [this, attempt, finish, descriptor, family, peer, lane, role, purpose, token] {
    boost::system::error_code error;
    attempt->socket.emplace(attempt->strand);
    attempt->socket->assign(Session::Socket::protocol_type(family, SOCK_STREAM), descriptor, error);
    if (error) {
      ::close(descriptor);
      attempt->socket.reset();
      return finish("adopt: " + error.message());
    }
    attempt->timer.expires_after(_options.connect_timeout);
    attempt->timer.async_wait([attempt](const boost::system::error_code& timer_error) {
      if (timer_error || attempt->finished) return;
      attempt->timed_out = true;
      attempt->abort();
    });
    auto handshaken = [this, attempt, finish, peer, role](std::exception_ptr refusal, Established established) {
      if (refusal) {
        attempt->abort();
        try {
          std::rethrow_exception(refusal);
        } catch (const std::exception& refused) {
          return finish(attempt->timed_out ? std::string("handshake timed out") : std::string("handshake: ") + refused.what());
        }
      }
      if (established.peer.id != peer) {
        attempt->abort();
        return finish("reached node " + to_string(established.peer.id) + ", not " + to_string(peer));
      }
      auto handlers = handlers_for(established.peer);
      auto session = Session::create(std::move(*attempt->socket), std::move(established), role, _options.routes, std::move(handlers), session_options());
      attempt->socket.reset();
      // Known before its first request can be read.
      if (_options.established) _options.established(session->established());
      session->start();
      if (role == Role::dialler)
        install_dialled(session);
      else
        admit(session);
      finish("");
    };
    if (role == Role::dialler)
      async_dial_handshake(*attempt->socket, _options.credentials, _options.local(), _options.max_frame, lane, handshaken, purpose, token);
    else
      async_accept_handshake(*attempt->socket, _options.credentials, _options.local(), _options.max_frame, Endpoint{}, handshaken,
                             Expected{purpose, token, peer});
  });
}

void Client::install_dialled(const std::shared_ptr<Session>& session) {
  const auto& peer = session->peer();
  const RouteKey key{peer.id, session->lane()};
  Retire retire;
  std::vector<Call> waiting;
  std::shared_ptr<asio::steady_timer> timer;
  std::shared_ptr<Session> route;
  {
    contract::Lock lock(_mutex);
    if (_stopped) {
      retire.push_back(session);
    } else {
      _peer_capabilities[peer.id] = peer.capabilities;
      auto& slot = _outbound[key];
      if (slot && slot->usable() && slot != session) {
        if (preferred(*session, *slot)) {
          retire.push_back(slot);
          slot = session;
          hold_locked(session);
        } else {
          retire.push_back(session);
        }
        ++_stats.retired;
      } else {
        slot = session;
        hold_locked(session);
      }
      reconcile_locked(key, retire);
      if (const auto found = _reverse.find(key); found != _reverse.end()) {
        route = route_locked(key.first, key.second);
        if (route) {
          waiting = std::move(found->second.calls);
          timer = found->second.timer;
          _reverse.erase(found);
        }
      }
    }
  }
  retire_sessions(retire);
  if (_options.established) _options.established(session->established());
  if (timer) asio::post(_health_strand, [timer] { timer->cancel(); });
  for (auto& call : waiting) place(route, std::move(call), Target{key.first, {peer.host, peer.port}});
}

std::optional<Purpose> Client::path_of(const identity::NodeId& peer, Lane lane) const {
  contract::Lock lock(_mutex);
  const RouteKey key{peer, lane};
  for (const auto* routes : {&_outbound, &_inbound}) {
    const auto found = routes->find(key);
    if (found != routes->end() && found->second->usable()) return found->second->established().purpose;
  }
  return std::nullopt;
}

std::vector<std::pair<identity::NodeId, Lane>> Client::routes_on(Purpose purpose) const {
  contract::Lock lock(_mutex);
  std::set<RouteKey> out;
  for (const auto* routes : {&_outbound, &_inbound})
    for (const auto& [key, session] : *routes)
      if (session->usable() && session->established().purpose == purpose) out.insert(key);
  return {out.begin(), out.end()};
}

bool Client::send(const identity::NodeId& peer, Lane lane, const Message& message, sched::FrameType frame, bool best_effort) {
  std::shared_ptr<Session> route;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return false;
    route = route_locked(peer, lane);
  }
  if (!route) return false;
  if (route->try_notify(message, frame)) return true;
  if (best_effort) return false;
  try {
    route->notify(message, frame, _io.get_executor());
    return true;
  } catch (const OutboundRefused&) {
    return false;
  }
}

void Client::open_lane(const membership::NodeRecord& peer, Lane lane) { ensure_lane(peer, lane); }

void Client::note_path_progress(const identity::NodeId& peer, Lane lane) {
  std::shared_ptr<Session> route;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return;
    route = route_locked(peer, lane);
  }
  if (route) route->note_path_progress();
}

Client::Paced Client::send_paced(const identity::NodeId& peer, Lane lane, Message message, sched::FrameType frame, asio::any_io_executor executor,
                                 std::function<void(bool)> sent) {
  std::shared_ptr<Session> route;
  {
    contract::Lock lock(_mutex);
    if (_stopped) return Paced::gone;
    route = route_locked(peer, lane);
  }
  if (!route) return Paced::gone;
  try {
    route->notify(std::move(message), frame, std::move(executor), std::move(sent));
    return Paced::taken;
  } catch (const OutboundRefused&) {
    return Paced::full;
  }
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
  auto out = _stats;
  out.canonical_connections = 0;
  for (const auto* routes : {&_outbound, &_inbound})
    for (const auto& [_, session] : *routes)
      if (session->usable()) ++out.canonical_connections;
  return out;
}

void Client::end_operation() {
  // Under the lock: stop() may return and this client go the moment the count is seen
  // at zero, and the condition variable with it.
  contract::Lock lock(_mutex);
  --_operations;
  _idle.notify_all();
}

void Client::observe_dial(const std::string& key, bool success) {
  contract::Lock lock(_mutex);
  observe_dial_locked(key, success);
}

void Client::observe_dial_locked(const std::string& key, bool success) {
  if (success) {
    _backoff.erase(key);
    return;
  }
  auto& backoff = _backoff[key];
  backoff.failures = std::min(backoff.failures + 1, 8u);
  backoff.retry_after = _options.time->now() + std::chrono::milliseconds(250) * (1u << std::min(backoff.failures - 1, 4u));
}

std::vector<std::shared_ptr<Session>> Client::control_routes_locked() const {
  std::vector<std::shared_ptr<Session>> out;
  for (const auto* routes : {&_outbound, &_inbound})
    for (const auto& [key, session] : *routes)
      if (key.second == Lane::control && session->usable()) out.push_back(session);
  return out;
}

size_t Client::broadcast(const Message& message) {
  std::vector<std::shared_ptr<Session>> routes;
  {
    contract::Lock lock(_mutex);
    routes = control_routes_locked();
  }
  size_t queued = 0;
  for (auto& session : routes) {
    try {
      session->notify(message, sched::FrameType::control, _io.get_executor());
      ++queued;
    } catch (const OutboundRefused&) {
    }
  }
  return queued;
}

size_t Client::broadcast_best_effort(const Message& message, sched::FrameType frame) { return notify_routes(message, frame, false); }

size_t Client::broadcast_notify(const Message& message, sched::FrameType frame) { return notify_routes(message, frame, true); }

size_t Client::notify_routes(const Message& message, sched::FrameType frame, bool wait) {
  std::vector<std::shared_ptr<Session>> routes;
  if (wait) {
    contract::Lock lock(_mutex);
    routes = control_routes_locked();
  } else {
    if (!_mutex.try_lock()) return 0;
    routes = control_routes_locked();
    _mutex.unlock();
  }
  size_t queued = 0;
  for (auto& session : routes)
    if (session->try_notify(message, frame)) ++queued;
  return queued;
}

void Client::hold_routes_for_test(const std::function<void()>& held) {
  contract::Lock lock(_mutex);
  held();
}

std::map<identity::NodeId, std::chrono::milliseconds> Client::peer_latencies() const {
  contract::Lock lock(_mutex);
  std::map<identity::NodeId, std::chrono::milliseconds> out;
  for (const auto& [peer, ms] : _latency_ms) out.emplace(peer, std::chrono::milliseconds(std::llround(ms)));
  return out;
}

std::optional<std::chrono::milliseconds> Client::peer_latency(const identity::NodeId& peer) const {
  contract::Lock lock(_mutex);
  const auto found = _latency_ms.find(peer);
  if (found == _latency_ms.end()) return std::nullopt;
  return std::chrono::milliseconds(std::llround(found->second));
}

// One peer's ping on one lane, for one heartbeat. On the health strand.
struct Client::Probe {
  std::weak_ptr<Session> session;
  membership::NodeRecord peer;
  Lane lane;
  contract::TimePoint deadline;
  asio::steady_timer timer;
  contract::TimePoint sent{};
  contract::TimePoint give_up_at{};
  uint64_t awaiting = 0;  // the ping this attempt waits on; 0 once answered
  bool stalled = false;
  bool expired = false;  // the deadline passed with a ping outstanding
  std::string last_error;
  // Held by every probe of a pass, so the pass is over when the last has gone.
  std::shared_ptr<void> pass;
  Probe(asio::strand<asio::io_context::executor_type>& strand, const std::shared_ptr<Session>& on, Lane via, contract::TimePoint until)
      : session(on), peer(on->peer()), lane(via), deadline(until), timer(strand) {}
};

void Client::start() {
  {
    contract::Lock lock(_mutex);
    if (_stopped || _health_started) return;
    _health_started = true;
    ++_operations;
  }
  asio::dispatch(_health_strand, [this] { arm_heartbeat(); });
}

void Client::arm_heartbeat() {
  _next_heartbeat = _options.time->now() + _options.heartbeat;
  await_heartbeat();
}

void Client::await_heartbeat() {
  _heartbeat.expires_after(sleep_towards(*_options.time, _next_heartbeat));
  _heartbeat.async_wait([this](const boost::system::error_code& error) {
    bool stopped;
    {
      contract::Lock lock(_mutex);
      stopped = _stopped;
    }
    if (error || stopped) return end_operation();
    if (_options.time->now() < _next_heartbeat) return await_heartbeat();
    open_lanes();
    health_pass();
    arm_heartbeat();
  });
}

void Client::health_pass() {
  // One pass at a time, as Macha's loop waits for its probes: a probe that
  // closes a dead route must not have a later pass's probe dial it again.
  if (!_health_pass.expired()) return;
  auto pass = std::make_shared<int>(0);
  _health_pass = pass;
  std::vector<std::shared_ptr<Probe>> probes;
  const auto deadline = _options.time->now() + _options.dead_after;
  {
    contract::Lock lock(_mutex);
    std::set<RouteKey> seen;
    // The route a call would take, on each lane there is one: a DATA lane is
    // probed too, so a dead NAT mapping is found before the next transfer,
    // but never dialled to be.
    for (const auto* routes : {&_outbound, &_inbound}) {
      for (const auto& [key, session] : *routes) {
        if (seen.contains(key) || !session->usable()) continue;
        seen.insert(key);
        probes.push_back(std::make_shared<Probe>(_health_strand, session, key.second, deadline));
        ++_operations;
      }
    }
  }
  for (auto& probe : probes) {
    probe->pass = pass;
    attempt(probe);
  }
}

void Client::attempt(const std::shared_ptr<Probe>& probe) {
  bool stopped;
  {
    contract::Lock lock(_mutex);
    stopped = _stopped;
  }
  if (stopped) return end_operation();
  const auto session = probe->session.lock();
  probe->stalled = false;
  probe->sent = _options.time->now();
  if (!session || !session->usable()) {
    // A DATA lane is never dialled to be probed. CONTROL's route went during
    // this probe: placing the ping dials it again, net.cpp:2975.
    if (probe->lane != Lane::control) return end_operation();
    return redial(probe);
  }
  {
    contract::Lock lock(_mutex);
    ++_stats.pings;
  }
  const auto admitted =
      session->call(message::ping, {}, sched::FrameType::control, _health_strand, [this, probe](CallOutcome outcome) { answered(probe, std::move(outcome)); });
  if (!admitted.refused.empty()) {
    probe->last_error = admitted.refused;
    return retry(probe);
  }
  // A third of the liveness budget per attempt, leaving room to retry.
  probe->give_up_at = std::min(probe->sent + std::max(_options.dead_after / 3, std::chrono::milliseconds(100)), probe->deadline);
  probe->awaiting = admitted.request_id;
  await_ping(probe);
}

void Client::redial(const std::shared_ptr<Probe>& probe) {
  {
    contract::Lock lock(_mutex);
    ++_stats.pings;
  }
  CallWatch watch;
  watch.no_progress = std::max(_options.dead_after / 3, std::chrono::milliseconds(100));
  watch.stall_notice = std::chrono::milliseconds(0);
  call(
      probe->peer, message::ping, {}, sched::FrameType::control, _health_strand,
      [this, probe](CallOutcome outcome) {
        const bool stalled = !outcome.reply && outcome.failure.find("made no progress") != std::string::npos;
        if (stalled && _options.time->now() >= probe->deadline) {
          _log.debug("peer " + probe->peer.host + ":" + std::to_string(probe->peer.port) +
                     " lane=control liveness failure: " + liveness_reason(probe->last_error));
          return end_operation();
        }
        probe->stalled = stalled;
        answered(probe, std::move(outcome));
      },
      watch);
}

void Client::await_ping(const std::shared_ptr<Probe>& probe) {
  probe->timer.expires_after(sleep_towards(*_options.time, probe->give_up_at));
  // Answered, the operation may have ended and this client gone: only the
  // probe is touched until `awaiting` says otherwise.
  probe->timer.async_wait([this, probe, id = probe->awaiting](const boost::system::error_code& error) {
    if (error || probe->awaiting != id) return;
    if (_options.time->now() < probe->give_up_at) return await_ping(probe);
    const auto live = probe->session.lock();
    // Its circuit moved since the ping went: the ping is behind those bytes,
    // not lost. The budgets start again from now.
    if (live && live->path_progress() > probe->sent) {
      probe->sent = live->path_progress();
      probe->give_up_at = _options.time->now() + std::max(_options.dead_after / 3, std::chrono::milliseconds(100));
      probe->deadline = std::max(probe->deadline, _options.time->now() + _options.dead_after);
      return await_ping(probe);
    }
    if (_options.time->now() >= probe->deadline) {
      // Closes exactly the route that owns this probe: a replacement the peer
      // opened meanwhile is not touched.
      probe->expired = true;
      if (live) {
        const auto reason = liveness_reason(probe->last_error);
        _log.debug("peer " + live->peer().host + ":" + std::to_string(live->peer().port) + " lane=" + lane_name(probe->lane) + " liveness failure: " + reason);
        live->close("liveness failure: " + reason);
      }
      return;
    }
    probe->stalled = true;
    if (live) live->cancel(id);
  });
}

void Client::answered(const std::shared_ptr<Probe>& probe, CallOutcome outcome) {
  probe->awaiting = 0;
  probe->timer.cancel();
  const auto session = probe->session.lock();
  // An answer is proof of life however late it comes.
  if (outcome.reply && outcome.reply->type == message::ok) {
    if (probe->lane == Lane::control) {
      const auto sample = std::chrono::duration<double, std::milli>(_options.time->now() - probe->sent).count();
      const auto peer = session ? session->peer() : probe->peer;
      {
        contract::Lock lock(_mutex);
        auto [slot, fresh] = _latency_ms.try_emplace(peer.id, sample);
        if (!fresh) slot->second = slot->second * 0.8 + sample * 0.2;
      }
      if (_options.alive) _options.alive(peer);
    }
    return end_operation();
  }
  if (probe->expired) return end_operation();
  // An attempt abandoned is not the peer abandoned: the retry still has most
  // of the liveness budget, on either lane.
  if (probe->stalled) {
    probe->last_error = "ping made no progress for " + std::to_string(std::max(_options.dead_after / 3, std::chrono::milliseconds(100)).count()) + " ms";
    return retry(probe);
  }
  probe->last_error = outcome.reply ? std::string("health probe rejected") : outcome.failure;
  // An error or a broken session: no longer a usable DATA lane, and CONTROL
  // decides whether the peer is dead.
  if (probe->lane == Lane::data) return end_operation();
  retry(probe);
}

void Client::retry(const std::shared_ptr<Probe>& probe) {
  constexpr auto pause = std::chrono::milliseconds(50);
  const auto resume_at = _options.time->now() + pause;
  if (resume_at >= probe->deadline) {
    // No ping outstanding, nothing to close on DATA: CONTROL decides whether
    // the peer is dead. Close exactly the route this probe was on: a
    // replacement the peer opened meanwhile is not touched.
    if (probe->lane == Lane::control) {
      const auto reason = liveness_reason(probe->last_error);
      _log.debug("peer " + probe->peer.host + ":" + std::to_string(probe->peer.port) + " liveness failure: " + reason);
      if (const auto session = probe->session.lock()) session->close("liveness failure: " + reason);
    }
    return end_operation();
  }
  await_retry(probe, resume_at);
}

void Client::await_retry(const std::shared_ptr<Probe>& probe, contract::TimePoint resume_at) {
  probe->timer.expires_after(sleep_towards(*_options.time, resume_at));
  probe->timer.async_wait([this, probe, resume_at](const boost::system::error_code& error) {
    bool stopped;
    {
      contract::Lock lock(_mutex);
      stopped = _stopped;
    }
    if (!error && !stopped && _options.time->now() < resume_at) return await_retry(probe, resume_at);
    attempt(probe);
  });
}

void Client::stop() {
  std::vector<std::shared_ptr<Session>> routes;
  std::vector<std::shared_ptr<Dialling>> dialling;
  std::map<std::string, std::vector<Call>> waiting;
  std::map<RouteKey, ReverseWait> reverse;
  std::vector<std::shared_ptr<asio::steady_timer>> timers;
  std::map<uint64_t, std::shared_ptr<Stalled>> stalled;
  bool again;
  {
    contract::Lock lock(_mutex);
    again = _stopped;
    _stopped = true;
    reverse.swap(_reverse);
    stalled.swap(_stalled);
    timers.assign(_reverse_timers.begin(), _reverse_timers.end());
    // The heartbeat's cancel below touches this object: counted, so stop()
    // waits for it.
    ++_operations;
    for (auto* table : {&_outbound, &_inbound}) {
      for (auto& [_, session] : *table) routes.push_back(session);
      table->clear();
    }
    // And those off the tables, retired by either end, which may still hold
    // calls waiting for an answer. Each once.
    for (const auto& weak : _held)
      if (auto session = weak.lock(); session && std::find(routes.begin(), routes.end(), session) == routes.end()) routes.push_back(std::move(session));
    _held.clear();
    dialling.assign(_dialling.begin(), _dialling.end());
    waiting.swap(_dials);
  }
  asio::post(_health_strand, [this] {
    _heartbeat.cancel();
    end_operation();
  });
  // Stopping twice, as a destructor after stop() does, says so once.
  if (!again) _log.debug("shutdown: RpcClient::stop begin");
  for (auto& session : routes) {
    session->close("client stopped");
    // Waited for: a read that completed before the close may still be delivering
    // into this client's owner.
    {
      contract::Lock lock(_mutex);
      ++_operations;
    }
    session->after_queued([this] { end_operation(); });
  }
  for (auto& attempt : dialling) asio::post(attempt->strand, [attempt] { attempt->abort(); });
  for (auto& [_, calls] : waiting)
    for (auto& call : calls) fail(call, "client stopped");
  for (auto& [_, wait] : reverse)
    for (auto& call : wait.calls) fail(call, "client stopped");
  for (auto& [_, held] : stalled)
    if (auto call = held->take()) fail(*call, "client stopped");
  for (auto& timer : timers) asio::post(_health_strand, [timer] { timer->cancel(); });
  {
    contract::Lock lock(_mutex);
    _idle.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return _operations == 0; });
  }
  if (!again) _log.debug("shutdown: RpcClient::stop complete connections=" + std::to_string(routes.size()));
}

}  // namespace dannan::net
