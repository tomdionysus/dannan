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
#include "dannan/net/session.h"

namespace dannan::net {

struct Endpoint {
  std::string host;
  uint16_t port = 0;
  auto operator<=>(const Endpoint&) const = default;
};

// Where requests that arrive on the sessions this client dialled go: the
// node's server scheduler, so every request is served by the same workers
// whichever end dialled.
struct InboundHandlers {
  std::function<void(const membership::NodeRecord& peer, Frame, Reply)> request;
  std::function<void(const identity::NodeId& peer, uint64_t request_id, sched::FrameType)> promoted;
  std::function<void(const identity::NodeId& peer, uint64_t request_id)> cancelled;
};

struct ClientOptions {
  identity::ClusterKey key;
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
  // Health: every heartbeat each peer with a route is pinged on each lane
  // it has; a ping that hangs is abandoned after a third of dead_after and
  // retried, and a CONTROL route with no answer for dead_after is closed.
  std::chrono::milliseconds heartbeat{5000};
  std::chrono::milliseconds dead_after{30000};
  // Told of each peer that answers a CONTROL ping, with its record.
  std::function<void(const membership::NodeRecord&)> alive;
};

struct ClientStats {
  uint64_t dials = 0;
  uint64_t dial_failures = 0;
  // Calls placed on a route that already existed.
  uint64_t reused = 0;
  // Sessions retired by reconciliation: a simultaneous dial's loser, or a
  // duplicate.
  uint64_t retired = 0;
};

// Calls peers over two lanes each, CONTROL and DATA: a lane is dialled on
// demand, reused, and shared both ways with the sessions peers opened to
// this node. Concurrent cold callers share one dial. When both ends have
// dialled, the session the lower node id dialled is kept and the other
// retired, so the two ends agree without a word. All public members are
// thread-safe; every completion is posted to the executor the caller passed.
class Client {
 public:
  using Done = std::function<void(CallOutcome)>;

  Client(boost::asio::io_context& io, ClientOptions options);
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // Calls `peer` on the lane its type travels on, dialling the peer's
  // advertised address if no route exists; a dial that reaches another node
  // fails. `done` is posted to `executor` exactly once. Waits on nothing.
  void call(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame, boost::asio::any_io_executor executor, Done done);
  // As above, to an address whose node is not known yet (a bootstrap).
  void call(const Endpoint& endpoint, MessageType type, Bytes payload, sched::FrameType frame, boost::asio::any_io_executor executor, Done done);

  // A session a peer opened to this node, from the server: a route back.
  void admit(const std::shared_ptr<Session>& session);

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
  };
  struct Target {
    std::optional<identity::NodeId> id;
    Endpoint endpoint;
  };
  struct Dialling;
  struct Probe;
  struct Backoff {
    unsigned failures = 0;
    contract::TimePoint retry_after{};
  };
  using RouteKey = std::pair<identity::NodeId, Lane>;
  using Retire = std::vector<std::shared_ptr<Session>>;

  void call(Target target, Call call);
  void place(const std::shared_ptr<Session>& session, Call call);
  static void fail(Call& call, std::string why);
  // Dials `target` on `lane`; `finished` is told the session or why not.
  void dial(Target target, Lane lane, std::function<void(std::shared_ptr<Session>, std::string)> finished);
  void dialled(const std::string& key, const Target& target, Lane lane, std::shared_ptr<Session> session, std::string why);
  std::shared_ptr<Session> route_locked(const identity::NodeId& peer, Lane lane) DANNAN_REQUIRES(_mutex);
  void reconcile_locked(const RouteKey& key, Retire& retire) DANNAN_REQUIRES(_mutex);
  void end_operation();
  // Records a dial's outcome against its address and lane: a failure backs
  // off 250 ms doubling to 4 s, a success clears it.
  void observe_dial(const std::string& key, bool success);
  size_t notify_routes(const Message& message, sched::FrameType frame, bool wait);
  std::vector<std::shared_ptr<Session>> control_routes_locked() const DANNAN_REQUIRES(_mutex);
  // The health loop, on its strand.
  void arm_heartbeat();
  void health_pass();
  void attempt(const std::shared_ptr<Probe>& probe);
  void answered(const std::shared_ptr<Probe>& probe, CallOutcome outcome);
  void retry(const std::shared_ptr<Probe>& probe);

  boost::asio::io_context& _io;
  ClientOptions _options;
  const identity::NodeId _self;
  boost::asio::strand<boost::asio::io_context::executor_type> _health_strand;
  boost::asio::steady_timer _heartbeat;

  mutable contract::Mutex _mutex;
  std::condition_variable _idle;
  std::map<RouteKey, std::shared_ptr<Session>> _outbound DANNAN_GUARDED_BY(_mutex);
  std::map<RouteKey, std::shared_ptr<Session>> _inbound DANNAN_GUARDED_BY(_mutex);
  // Dials in flight by peer (or address) and lane, with the calls waiting
  // on each.
  std::map<std::string, std::vector<Call>> _dials DANNAN_GUARDED_BY(_mutex);
  std::map<Endpoint, identity::NodeId> _endpoint_ids DANNAN_GUARDED_BY(_mutex);
  std::set<std::shared_ptr<Dialling>> _dialling DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, Backoff> _backoff DANNAN_GUARDED_BY(_mutex);
  std::map<identity::NodeId, double> _latency_ms DANNAN_GUARDED_BY(_mutex);
  bool _health_started DANNAN_GUARDED_BY(_mutex) = false;
  ClientStats _stats DANNAN_GUARDED_BY(_mutex);
  size_t _operations DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopped DANNAN_GUARDED_BY(_mutex) = false;
};

}  // namespace dannan::net
