//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/membership/identity_reset.h"
#include "dannan/net/scheduler.h"
#include "dannan/net/session.h"
#include "dannan/status/log.h"

namespace dannan::net {

// Where requests that arrive on the sessions this client dialled go: the
// node's server scheduler, so every request is served by the same workers
// whichever end dialled.
struct InboundHandlers {
  std::function<void(const membership::NodeRecord& peer, Frame, Reply)> request;
  std::function<void(const identity::NodeId& peer, uint64_t request_id, sched::FrameType)> promoted;
  std::function<void(const identity::NodeId& peer, uint64_t request_id)> cancelled;
};

// A call placed through the client: control over it while it is pending.
// Thread-safe; every operation waits on nothing.
class CallHandle {
 public:
  // Completes the call at once as cancelled and tells the peer; before the
  // dial finished, the call is never placed.
  void cancel();
  // Cancels and ends the session it is on: the peer's job may finish, its
  // reply is dropped.
  void abort();
  void promote(sched::FrameType frame);
  // How long the call has gone without a byte of its reply; max() while it
  // is not placed, and once it is done.
  std::chrono::milliseconds idle_for() const;

 private:
  friend class Client;
  void placed(const std::shared_ptr<Session>& session, uint64_t request_id);
  // True if cancelled before placement, so the caller fails the call.
  bool cancelled_before_placement() const;

  mutable contract::Mutex _mutex;
  std::weak_ptr<Session> _session DANNAN_GUARDED_BY(_mutex);
  uint64_t _id DANNAN_GUARDED_BY(_mutex) = 0;
  bool _cancelled DANNAN_GUARDED_BY(_mutex) = false;
  bool _aborted DANNAN_GUARDED_BY(_mutex) = false;
  std::optional<sched::FrameType> _promoted DANNAN_GUARDED_BY(_mutex);
};

// How one call is watched for progress, in place of its type's and its lane's
// settings: a deadline cancels it, a notice only logs. Macha watches a synchronous
// call and leaves an asynchronous one to its caller, so a call says which it is.
struct CallWatch {
  std::optional<std::chrono::milliseconds> no_progress = std::nullopt;
  std::optional<std::chrono::milliseconds> stall_notice = std::nullopt;
};

struct ClientOptions {
  // How this node proves itself and checks its peers.
  Credentials credentials;
  // The node's current record, read at each dial.
  std::function<membership::NodeRecord()> local;
  size_t max_frame = 256 * 1024;
  std::shared_ptr<const RouteTable> routes;
  sched::RetainedMemoryLedger* memory = nullptr;
  // Shared with the node's server: the node's whole cluster traffic.
  std::shared_ptr<TransportTraffic> traffic;
  const contract::TimeSource* time = &contract::steady_time_source();
  InboundHandlers inbound;
  // Bounds a dial (resolve, connect, handshake), and a call's wait for one.
  std::chrono::milliseconds connect_timeout{5000};
  // A call that has received no byte of its reply for this long is
  // cancelled and fails transiently, for the caller to retry; zero waits for
  // ever. Per lane, since a DATA reply may legitimately take long to start.
  std::chrono::milliseconds control_no_progress{0};
  std::chrono::milliseconds data_no_progress{0};
  // Logged at debug, again each period, while a call makes no progress; zero
  // for none. A type's registration may name its own, as for the deadlines.
  std::chrono::milliseconds control_stall_notice{0};
  std::chrono::milliseconds data_stall_notice{0};
  // Where the client writes its journal lines; none, nowhere.
  std::shared_ptr<status::LogSink> log;
  // Health: every heartbeat each peer with a route is pinged on each lane
  // it has; a ping that hangs is abandoned after a third of dead_after and
  // retried, and a CONTROL route with no answer for dead_after is closed.
  std::chrono::milliseconds heartbeat{5000};
  std::chrono::milliseconds dead_after{30000};
  // Told of each peer that answers a CONTROL ping, with its record.
  std::function<void(const membership::NodeRecord&)> alive;
  // Told of each session this client dials or adopts, as its handshake
  // established it: the peer's record (a direct observation) and the
  // certificate it proved, if any. Told before the session reads, so before
  // the client knows whether it is stopping: it may be told of a session that
  // closes at once as "client stopped". Must not wait.
  std::function<void(const Established&)> established;
  // Tests only: consulted before every dial; a non-empty answer is the
  // reason the dial fails, as if the network refused it.
  std::function<std::string(const Endpoint&, Lane)> dial_hook;
  // Called with the session just before a call is placed on it, for a test that
  // retires it there.
  std::function<void(Session&)> place_hook;
  // Level 0: asked for a path to a peer that can be neither dialled nor
  // asked to dial back (it takes no inbound connection, and neither does
  // this node, or it has no session here to ask over). The maker hands the
  // path to adopt() when it has one, or calls `failed` with why not; the
  // calls wait for it within path_timeout. Null: such calls fail at once.
  std::function<void(const identity::NodeId& peer, Lane lane, std::function<void(std::string)> failed)> make_path;
  std::chrono::milliseconds path_timeout{15000};
};

struct ClientStats {
  uint64_t dials = 0;
  // Dials that became sessions, counted once the peer's identity has been checked.
  uint64_t connections_created = 0;
  // Routes usable now, whichever end dialled them: a lane to a peer counts once.
  uint64_t canonical_connections = 0;
  uint64_t dial_failures = 0;
  // Health pings sent, retries included.
  uint64_t pings = 0;
  // Calls placed on a route that already existed.
  uint64_t reused = 0;
  // Sessions retired by reconciliation: a simultaneous dial's loser, or a
  // duplicate.
  uint64_t retired = 0;
  // Lanes asked of inbound-incapable peers, and asked of this node.
  uint64_t dial_requests_sent = 0;
  uint64_t dial_requests_received = 0;
  // Level 0's paths asked for, and sessions adopted on them.
  uint64_t paths_requested = 0;
  uint64_t paths_adopted = 0;
};

// Calls peers over two lanes each, CONTROL and DATA: a lane is dialled on
// demand, reused, and shared both ways with the sessions peers opened to
// this node. Concurrent cold callers share one dial. When both ends have
// dialled, the session the lower node id dialled is kept and the other
// retired, so the two ends agree without a word.
//
// A node that accepts no inbound connection (its record's inbound_capable
// flag clear) is never dialled. Peers reach it over the sessions it opened;
// a lane it has not opened is asked for with dial_request over its CONTROL
// session, and the call waits for the reverse dial within the connect
// timeout. Such a node keeps both lanes dialled to every maintained peer,
// since only it can restore its own reachability.
//
// All public members are thread-safe; every completion is posted to the
// executor the caller passed. `local` is called with the route table held,
// so it must not call into the client.
class Client {
 public:
  using Done = std::function<void(CallOutcome)>;

  Client(boost::asio::io_context& io, ClientOptions options);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Calls `peer` on the lane its type travels on, dialling the peer's
  // advertised address if no route exists; a dial that reaches another node
  // fails. `done` is posted to `executor` exactly once. The handle controls
  // the call while it is pending. Waits on nothing.
  std::shared_ptr<CallHandle> call(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame,
                                   boost::asio::any_io_executor executor, Done done, CallWatch watch = {});
  // As above, to an address whose node is not known yet (a bootstrap).
  std::shared_ptr<CallHandle> call(const Endpoint& endpoint, MessageType type, Bytes payload, sched::FrameType frame, boost::asio::any_io_executor executor,
                                   Done done, CallWatch watch = {});

  // A session a peer opened to this node, from the server: a route back.
  void admit(const std::shared_ptr<Session>& session);
  // A connected stream that a level 0 path made to `peer` (punched or
  // relayed), given as its descriptor, which the client owns from here. The
  // handshake runs as `role` with `purpose` and `token` within the connect
  // timeout, the other end must be `peer`, and the session becomes an
  // ordinary route, placing the calls that waited for one. `done`, if
  // given, is posted with why not, empty on success.
  void adopt(int descriptor, int family, const identity::NodeId& peer, Lane lane, Role role, Purpose purpose, PathToken token,
             std::function<void(std::string)> done = {});
  // Why the route to `peer` on `lane` was opened (ordinary, punched or
  // relayed); nothing without a route.
  std::optional<Purpose> path_of(const identity::NodeId& peer, Lane lane) const;
  // Every route whose session was opened for `purpose`, by peer and lane.
  std::vector<std::pair<identity::NodeId, Lane>> routes_on(Purpose purpose) const;
  // A notification to `peer` over the route it has on `lane`; never dials.
  // False without a route, or when the session refuses it; with
  // `best_effort`, also when it would have to queue behind other traffic.
  bool send(const identity::NodeId& peer, Lane lane, const Message& message, sched::FrameType frame, bool best_effort = false);
  // The same for a sender that paces itself and must lose nothing. Says
  // whether the session took it, its queue is full for now, or there is no
  // route; when taken, `sent` is told on `executor` whether it was written.
  enum class Paced { taken, full, gone };
  Paced send_paced(const identity::NodeId& peer, Lane lane, Message message, sched::FrameType frame, boost::asio::any_io_executor executor,
                   std::function<void(bool)> sent);

  // Starts the health pings.
  void start();

  // Notifies every CONTROL route, whichever end dialled it; never dials.
  // Returns how many took it.
  size_t broadcast(const Message& message);
  // As broadcast, at `frame`, but only where it adds no delay
  // (Session::try_notify); sends nothing if the route table is momentarily
  // in use, so a caller on a hot path never waits.
  size_t broadcast_best_effort(const Message& message, sched::FrameType frame);
  // As broadcast_best_effort, but waits for the route table. Still never
  // dials and never waits on a peer.
  size_t broadcast_notify(const Message& message, sched::FrameType frame);
  // Runs `held` while holding the route table. Tests only: the one way to
  // show best effort skipping a busy table.
  void hold_routes_for_test(const std::function<void()>& held);

  // The smoothed CONTROL round trip to a peer, from health pings only.
  std::optional<std::chrono::milliseconds> peer_latency(const identity::NodeId& peer) const;
  std::map<identity::NodeId, std::chrono::milliseconds> peer_latencies() const;

  // A one-shot dial on the probe lane: the handshake must authenticate as
  // `expected`, then the connection closes; never a route. `done` gets the
  // reason it failed, empty on success.
  void probe(const Endpoint& endpoint, const identity::NodeId& expected, boost::asio::any_io_executor executor, std::function<void(std::string)> done);

  // Teaches the client a peer's flags (inbound-capable or not) before a
  // handshake has: membership calls it from the roster.
  void note_peer(const membership::NodeRecord& peer);
  // While this node is inbound-incapable, both lanes are kept dialled to
  // every capable peer `peers` returns, checked each heartbeat.
  void set_maintained_peers(std::function<std::vector<membership::NodeRecord>()> peers);
  // A peer asked this node to dial `lane` to it; dialled at once, under the
  // ordinary backoff, so a peer that keeps losing a lane cannot cause a
  // redial loop.
  void request_lane(const membership::NodeRecord& peer, Lane lane);
  // Handles a dial_request notification from `peer` (the node routes the
  // type here): processed, or failed with 400 for a lane that is not
  // CONTROL or DATA.
  Handled handle_dial_request(const membership::NodeRecord& peer, const Message& request);
  // An identity reset: forgets the addresses it covers and closes the
  // routes to the nodes it names there, so the next call dials afresh and
  // checks who answers.
  void invalidate_identity_association(const membership::IdentityReset& reset);
  // Closes this node's routes to `peer` on `lane`, both directions.
  void close_route(const identity::NodeId& peer, Lane lane, const std::string& why);
  // The path under the route to `peer` on `lane` moved the far end's bytes:
  // its pings are waiting behind them, so the peer is alive.
  void note_path_progress(const identity::NodeId& peer, Lane lane);
  // Starts dialling `lane` to `peer` unless a route or a dial is there already.
  // Never waits.
  void open_lane(const membership::NodeRecord& peer, Lane lane);

  bool has_route(const identity::NodeId& peer, Lane lane) const;
  ClientStats stats() const;
  // The lane `type` travels on: DATA for a type registered only there.
  Lane lane_for(MessageType type) const;
  const std::shared_ptr<TransportTraffic>& traffic() const noexcept { return _options.traffic; }

  // Fails every pending call at once, closes every route, and admits
  // nothing more. Waits for the dials in flight to end. Must not run on the
  // io context's only thread.
  void stop();

 private:
  struct Call {
    MessageType type{};
    Bytes payload;
    sched::FrameType frame = sched::FrameType::control;
    boost::asio::any_io_executor executor;
    Done done;
    // Times this call has been re-routed off a session that retired or
    // closed under it.
    int reroutes = 0;
    std::shared_ptr<CallHandle> handle;
    CallWatch watch{};
  };
  struct Target {
    std::optional<identity::NodeId> id;
    Endpoint endpoint;
  };
  struct Dialling;
  struct Probe;
  // Calls waiting for an inbound-incapable peer to dial a lane back, or
  // for a level 0 path to it.
  struct ReverseWait {
    std::vector<Call> calls;
    std::shared_ptr<boost::asio::steady_timer> timer;
    std::string backoff_key;
    bool path = false;
    // Waiting for the peer's own session, after a dial of this end's was retired.
    bool peers_session = false;
  };
  struct Backoff {
    unsigned failures = 0;
    contract::TimePoint retry_after{};
  };
  using RouteKey = std::pair<identity::NodeId, Lane>;
  using Retire = std::vector<std::shared_ptr<Session>>;

  void call(Target target, Call call);
  // Places `call` on `session`; one refused because the session retired or
  // closed under it is routed again (a simultaneous dial's loser), at most
  // a few times. `target` is where it is going.
  void place(const std::shared_ptr<Session>& session, Call call, const Target& target);
  static void fail(Call& call, std::string why);
  // Dials `target` on `lane`; `finished` is told the session or why not.
  void dial(Target target, Lane lane, std::function<void(std::shared_ptr<Session>, std::string)> finished);
  void dialled(const std::string& key, const Target& target, Lane lane, std::shared_ptr<Session> session, std::string why);
  std::shared_ptr<Session> route_locked(const identity::NodeId& peer, Lane lane) DANNAN_REQUIRES(_mutex);
  void reconcile_locked(const RouteKey& key, Retire& retire) DANNAN_REQUIRES(_mutex);
  // Retires the losers, remembering them so stop() closes them too.
  void retire_sessions(Retire& retire) DANNAN_EXCLUDES(_mutex);
  void end_operation();
  // Records a dial's outcome against its address and lane: a failure backs
  // off 250 ms doubling to 4 s, a success clears it.
  void observe_dial(const std::string& key, bool success);
  size_t notify_routes(const Message& message, sched::FrameType frame, bool wait);
  std::vector<std::shared_ptr<Session>> control_routes_locked() const DANNAN_REQUIRES(_mutex);
  // The health loop, on its strand.
  void arm_heartbeat();
  void await_heartbeat();
  void health_pass();
  void attempt(const std::shared_ptr<Probe>& probe);
  void redial(const std::shared_ptr<Probe>& probe);
  void answered(const std::shared_ptr<Probe>& probe, CallOutcome outcome);
  void retry(const std::shared_ptr<Probe>& probe);
  void await_ping(const std::shared_ptr<Probe>& probe);
  void await_retry(const std::shared_ptr<Probe>& probe, contract::TimePoint resume_at);
  // Lanes a peer asked for, and those an incapable node maintains. On the
  // health strand.
  void open_lanes();
  // Dials `peer` on `lane` with no call waiting, unless a route, a dial or
  // a backoff is already there.
  void ensure_lane(const membership::NodeRecord& peer, Lane lane);
  bool capable_locked(const identity::NodeId& peer) const DANNAN_REQUIRES(_mutex);
  bool local_capable() const;
  // Asks an incapable peer for `key`'s lane over `control`, and arms the
  // wait for its reverse dial.
  void await_peers_session(Target target, Lane lane, Call call);
  void ask_for_lane(const RouteKey& key, const std::shared_ptr<Session>& control, const std::shared_ptr<boost::asio::steady_timer>& timer);
  void reverse_expired(const RouteKey& key, const std::shared_ptr<boost::asio::steady_timer>& timer);
  // Asks the path maker for `key`, the wait already armed.
  void ask_for_path(const RouteKey& key, const std::shared_ptr<boost::asio::steady_timer>& timer);
  // Fails the calls waiting on `key` under `timer`, with `why`.
  void abandon_wait(const RouteKey& key, const std::shared_ptr<boost::asio::steady_timer>& timer, const std::string& why);
  // How a session reaches the node's workers, and its options.
  SessionHandlers handlers_for(const membership::NodeRecord& peer) const;
  SessionOptions session_options() const;
  // A session this end dialled over an adopted path: installed as dialled,
  // and the calls waiting for it placed.
  void install_dialled(const std::shared_ptr<Session>& session);

  boost::asio::io_context& _io;
  ClientOptions _options;
  const status::ScopedLogger _log;
  const identity::NodeId _self;
  boost::asio::strand<boost::asio::io_context::executor_type> _health_strand;
  // The health pass running; on the health strand.
  std::weak_ptr<void> _health_pass;
  boost::asio::steady_timer _heartbeat;
  contract::TimePoint _next_heartbeat{};  // on the health strand

  mutable contract::Mutex _mutex;
  std::condition_variable _idle;
  std::map<RouteKey, std::shared_ptr<Session>> _outbound DANNAN_GUARDED_BY(_mutex);
  std::map<RouteKey, std::shared_ptr<Session>> _inbound DANNAN_GUARDED_BY(_mutex);
  // Sessions retired off the tables but not yet closed: they may hold calls.
  std::vector<std::weak_ptr<Session>> _retiring DANNAN_GUARDED_BY(_mutex);
  // Dials in flight by peer (or address) and lane, with the calls waiting
  // on each.
  std::map<std::string, std::vector<Call>> _dials DANNAN_GUARDED_BY(_mutex);
  std::map<Endpoint, identity::NodeId> _endpoint_ids DANNAN_GUARDED_BY(_mutex);
  std::set<std::shared_ptr<Dialling>> _dialling DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, Backoff> _backoff DANNAN_GUARDED_BY(_mutex);
  std::map<identity::NodeId, double> _latency_ms DANNAN_GUARDED_BY(_mutex);
  bool _health_started DANNAN_GUARDED_BY(_mutex) = false;
  // Each peer's flags, from handshakes, calls and note_peer(); a peer not
  // listed is taken as capable.
  std::map<identity::NodeId, uint32_t> _peer_capabilities DANNAN_GUARDED_BY(_mutex);
  std::map<RouteKey, membership::NodeRecord> _requested DANNAN_GUARDED_BY(_mutex);
  std::function<std::vector<membership::NodeRecord>()> _maintained DANNAN_GUARDED_BY(_mutex);
  std::map<RouteKey, ReverseWait> _reverse DANNAN_GUARDED_BY(_mutex);
  std::set<std::shared_ptr<boost::asio::steady_timer>> _reverse_timers DANNAN_GUARDED_BY(_mutex);
  ClientStats _stats DANNAN_GUARDED_BY(_mutex);
  size_t _operations DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopped DANNAN_GUARDED_BY(_mutex) = false;
};

}  // namespace dannan::net
