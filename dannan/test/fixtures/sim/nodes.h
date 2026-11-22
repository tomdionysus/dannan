//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dannan/identity/cluster_key.h"
#include "dannan/net/transport.h"
#include "dannan/node.h"
#include "dannan/test/fixtures/recording_sink.h"
#include "dannan/test/fixtures/sim/agreement.h"
#include "dannan/test/fixtures/sim/world.h"
#include "dannan/test/fixtures/temp_dir.h"

// Whole nodes in the world (the harness's step 3 part 4, piece 2b: the transport seam,
// `2026-11-17-sim-transport-seam.md`): each a real dannan::Node on the world's transport,
// time and schedule, so its membership, routes, placement, channels and agreement run under
// the seed. Sessions, TLS and the scheduler stay outside (Mimir's and Kvasir's reviews).
namespace dannan::test::sim {

// One node's clocks: the world's time at its own offset and drift (Mimir's first ask), steady
// and wall alike. Drift is in parts per million.
class NodeClock final : public contract::TimeSource, public contract::WallClock {
 public:
  NodeClock(const World& world, int64_t offset_ms, int64_t drift_ppm) noexcept : _world(world), _offset(offset_ms), _drift(drift_ppm) {}
  contract::TimePoint now() const noexcept override { return contract::TimePoint(std::chrono::milliseconds(local())); }
  std::chrono::milliseconds wake_within(std::chrono::milliseconds remaining) const noexcept override { return remaining; }
  uint64_t unix_ms() const noexcept override { return 1'790'000'000'000ull + local(); }
  // Moves this node's clocks on by `by_ms`, at once, as a clock stepped by NTP or an operator.
  // Forward only: the steady clock moves with the wall, and a steady clock must never go back.
  void step(int64_t by_ms) noexcept {
    if (by_ms > 0) _offset.fetch_add(by_ms, std::memory_order_relaxed);
  }

 private:
  uint64_t local() const noexcept {
    const auto at = static_cast<int64_t>(_world.now_ms());
    return static_cast<uint64_t>(at + at / 1'000'000 * _drift + (at % 1'000'000) * _drift / 1'000'000 + _offset.load(std::memory_order_relaxed));
  }
  const World& _world;
  // Atomic: a node's own threads read it while a test steps it.
  std::atomic<int64_t> _offset;
  int64_t _drift;
};

class SimNetwork;

// One node's transport through the world: every message an event, delayed, lost, duplicated or
// blocked as the network's faults say.
class SimTransport final : public net::Transport, public std::enable_shared_from_this<SimTransport> {
 public:
  SimTransport(SimNetwork& network, size_t index, net::TransportHooks hooks) : _network(network), _index(index), _hooks(std::move(hooks)) {}

  void listen() override;
  // Health pings on every route each `ping_every`, as the real client sends them: a peer that
  // answers is seen, whatever membership's own exchanges are waiting on.
  void start() override { ping_later(); }
  uint16_t bound_port() const override;
  void stop_serving() override { _serving = false; }
  void stop_calling() override { _calling = false; }

  std::shared_ptr<net::CallHandle> call(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                        boost::asio::any_io_executor executor, net::Client::Done done, net::CallWatch = {}) override;
  std::shared_ptr<net::CallHandle> call(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                        boost::asio::any_io_executor executor, net::Client::Done done, net::CallWatch = {}) override;
  bool send(const identity::NodeId& peer, net::Lane, const net::Message& message, sched::FrameType frame, bool = false) override;
  size_t broadcast(const net::Message& message) override { return broadcast_notify(message, sched::FrameType::control); }
  size_t broadcast_best_effort(const net::Message& message, sched::FrameType frame) override { return broadcast_notify(message, frame); }
  size_t broadcast_notify(const net::Message& message, sched::FrameType frame) override;

  bool has_route(const identity::NodeId& peer, net::Lane) const override;
  void close_route(const identity::NodeId& peer, net::Lane, const std::string&) override;
  void note_peer(const membership::NodeRecord&) override {}
  void set_maintained_peers(std::function<std::vector<membership::NodeRecord>()>) override {}
  void invalidate_identity_association(const membership::IdentityReset&) override {}

  net::ClientStats stats() const override {
    net::ClientStats out;
    out.canonical_connections = _routes.size();
    return out;
  }
  net::WorkStats work_stats() const override { return {}; }
  std::optional<std::chrono::milliseconds> peer_latency(const identity::NodeId&) const override { return std::nullopt; }
  std::map<identity::NodeId, std::chrono::milliseconds> peer_latencies() const override { return {}; }
  std::optional<net::Purpose> path_of(const identity::NodeId&, net::Lane) const override { return std::nullopt; }
  size_t release_stalled(const std::optional<identity::NodeId>&) override { return 0; }
  size_t stalled() const override { return 0; }

  // The network's side.
  membership::NodeRecord local() const { return _hooks.local(); }
  // This node's record as the node at `index` holds it from their session: what it was when the
  // route was first used, as a session's handshake carries it and its calls carry nothing newer.
  membership::NodeRecord sent_as(size_t index) {
    auto found = _sent_as.find(index);
    if (found == _sent_as.end()) found = _sent_as.emplace(index, local()).first;
    return found->second;
  }
  bool serving() const noexcept { return _serving; }
  bool calling() const noexcept { return _calling; }
  // A request or notification from `from`, delivered: served as the server would, its reply
  // (none for a notification) told through `reply`.
  void serve(const membership::NodeRecord& from, sched::FrameType frame, net::Message request, std::function<void(net::Message)> reply);
  void heard(const membership::NodeRecord& from) {
    _routes.insert(from.id);
    if (_hooks.heard) _hooks.heard(from);
  }

 private:
  void ping_later();
  std::shared_ptr<net::CallHandle> place(std::optional<size_t> to, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                         boost::asio::any_io_executor executor, net::Client::Done done);

  SimNetwork& _network;
  size_t _index;
  net::TransportHooks _hooks;
  bool _serving = false;
  bool _calling = true;
  std::set<identity::NodeId> _routes;
  // This node's record per peer, as each session's handshake carried it (sent_as()).
  std::map<size_t, membership::NodeRecord> _sent_as;
};

// The world's network for whole nodes: the faults of NetworkFaults, one-way blocks, and
// partitions on the node matrix with a heal at a chosen tick (Mimir's third ask).
class SimNetwork {
 public:
  SimNetwork(World& world, NetworkFaults faults) : _world(world), _faults(faults) {}
  SimNetwork(const SimNetwork&) = delete;
  SimNetwork& operator=(const SimNetwork&) = delete;

  World& world() noexcept { return _world; }
  NetworkFaults& faults() noexcept { return _faults; }
  // How often each transport pings its routes: the nodes' heartbeat.
  uint64_t ping_every_ms = 100;

  // The port node `index` listens on; its transport, made by the node through NodeConfig.
  static uint16_t port_of(size_t index) noexcept { return static_cast<uint16_t>(20000 + index); }
  std::function<std::shared_ptr<net::Transport>(net::TransportHooks)> transport_for(size_t index) {
    return [this, index](net::TransportHooks hooks) {
      auto transport = std::make_shared<SimTransport>(*this, index, std::move(hooks));
      if (_transports.size() <= index) _transports.resize(index + 1);
      _transports[index] = transport;
      return transport;
    };
  }

  // Messages from `from` to `to` lost until unblocked.
  void block(size_t from, size_t to) { _blocked.insert({from, to}); }
  // A clean split: nobody in one group reaches anybody in another.
  void partition(const std::vector<std::vector<size_t>>& groups) {
    for (size_t a = 0; a < groups.size(); ++a)
      for (size_t b = 0; b < groups.size(); ++b)
        if (a != b)
          for (const auto from : groups[a])
            for (const auto to : groups[b]) block(from, to);
  }
  void heal() { _blocked.clear(); }

  // Delivery, for the transports.
  std::optional<size_t> index_of(uint16_t port) const {
    for (size_t i = 0; i < _transports.size(); ++i)
      if (port_of(i) == port) return i;
    return std::nullopt;
  }
  std::optional<size_t> index_of(const identity::NodeId& id) const {
    for (size_t i = 0; i < _transports.size(); ++i)
      if (const auto transport = _transports[i].lock(); transport && transport->local().id == id) return i;
    return std::nullopt;
  }
  std::shared_ptr<SimTransport> at(size_t index) const { return index < _transports.size() ? _transports[index].lock() : nullptr; }
  size_t size() const noexcept { return _transports.size(); }
  bool lost(size_t from, size_t to) { return _blocked.contains({from, to}) || _world.random.one_in(_faults.drop_one_in); }
  // Whether `from` is cut off from `to` by a block or a partition: no random draw, so asking
  // leaves the seed's sequence as it was.
  bool blocked(size_t from, size_t to) const { return _blocked.contains({from, to}); }
  uint64_t delay() { return 1 + _world.random.below(_faults.max_delay_ms); }
  int copies() { return _world.random.one_in(_faults.duplicate_one_in) ? 2 : 1; }

 private:
  World& _world;
  NetworkFaults _faults;
  std::vector<std::weak_ptr<SimTransport>> _transports;
  std::set<std::pair<size_t, size_t>> _blocked;
};

inline void SimTransport::listen() { _serving = true; }
inline uint16_t SimTransport::bound_port() const { return SimNetwork::port_of(_index); }

inline void SimTransport::serve(const membership::NodeRecord& from, sched::FrameType frame, net::Message request, std::function<void(net::Message)> reply) {
  if (!_serving) return;
  heard(from);
  // Answered once: by the handler's return, or later through the Answer it kept. An Answer
  // dropped after the handler answered by returning sends nothing more.
  auto told = std::make_shared<bool>(false);
  auto give = [told, reply](net::Message message) {
    if (*told) return;
    *told = true;
    if (reply) reply(std::move(message));
  };
  net::Handled handled = net::Handled::unrecognised();
  {
    auto answer = net::Answer::to(net::Reply(give));
    try {
      handled = _hooks.serve(from, frame, std::move(request), answer);
    } catch (const std::exception& error) {
      handled = net::Handled::failed(500, "internal", error.what());
    }
    if (handled.outcome() == net::Handled::Outcome::deferred) return;
    *told = true;
  }
  if (!reply) return;
  if (handled.outcome() == net::Handled::Outcome::unrecognised) return reply(net::error_reply(501, "unrecognised", "no handler for this type"));
  reply(handled.take_reply());
}

inline std::shared_ptr<net::CallHandle> SimTransport::place(std::optional<size_t> to, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                                            boost::asio::any_io_executor executor, net::Client::Done done) {
  auto handle = std::make_shared<net::CallHandle>();
  if (!_calling) {
    boost::asio::post(executor, [done = std::move(done)] { done({std::nullopt, "the transport has stopped", {}}); });
    return handle;
  }
  // Unknown, or lost on the way: the caller's deadline ends it.
  if (!to) return handle;
  auto once = std::make_shared<bool>(false);
  const auto from = _index;
  auto& network = _network;
  const auto sender = sent_as(*to);
  // The reply goes to this transport, not to whoever holds its index by then: a node restarted
  // there is another node, and this one, and what its calls complete into, may be gone.
  const std::weak_ptr<SimTransport> self = weak_from_this();
  const int copies = network.copies();
  for (int copy = 0; copy < copies; ++copy) {
    if (network.lost(from, *to)) continue;
    network.world().after(network.delay(), [&network, from, to = *to, sender, type, payload, frame, executor, done, once, self] {
      const auto server = network.at(to);
      if (!server || !server->serving()) return;
      server->serve(sender, frame, net::Message{type, payload, {}}, [&network, from, to, executor, done, once, self](net::Message reply) {
        if (network.lost(to, from)) return;
        network.world().after(network.delay(), [&network, from, to, executor, done, once, self, reply = std::move(reply)]() mutable {
          const auto caller = self.lock();
          const auto answerer = network.at(to);
          if (!caller || !caller->calling() || *once) return;
          *once = true;
          const auto record = answerer ? answerer->sent_as(from) : membership::NodeRecord{};
          if (answerer) caller->heard(record);
          boost::asio::post(executor, [done, reply = std::move(reply), record = std::make_shared<const membership::NodeRecord>(record)]() mutable {
            done({std::move(reply), {}, record});
          });
        });
      });
    });
  }
  return handle;
}

inline std::shared_ptr<net::CallHandle> SimTransport::call(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload,
                                                           sched::FrameType frame, boost::asio::any_io_executor executor, net::Client::Done done,
                                                           net::CallWatch) {
  auto to = _network.index_of(peer.id);
  if (!to) to = _network.index_of(peer.port);
  return place(to, type, std::move(payload), frame, std::move(executor), std::move(done));
}

inline std::shared_ptr<net::CallHandle> SimTransport::call(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                                           boost::asio::any_io_executor executor, net::Client::Done done, net::CallWatch) {
  return place(_network.index_of(endpoint.port), type, std::move(payload), frame, std::move(executor), std::move(done));
}

inline void SimTransport::ping_later() {
  const std::weak_ptr<SimTransport> self = weak_from_this();
  _network.world().after(_network.ping_every_ms, [self] {
    const auto me = self.lock();
    if (!me || !me->calling()) return;
    for (const auto& peer : std::vector<identity::NodeId>(me->_routes.begin(), me->_routes.end()))
      if (const auto to = me->_network.index_of(peer))
        (void)me->place(to, net::message::ping, {}, sched::FrameType::control, me->_network.world().io.get_executor(), [](net::CallOutcome) {});
    me->ping_later();
  });
}

inline bool SimTransport::send(const identity::NodeId& peer, net::Lane, const net::Message& message, sched::FrameType frame, bool) {
  const auto to = _network.index_of(peer);
  if (!_calling || !to || !has_route(peer, net::Lane::control)) return false;
  const auto from = _index;
  auto& network = _network;
  const auto sender = sent_as(*to);
  const int copies = network.copies();
  for (int copy = 0; copy < copies; ++copy) {
    if (network.lost(from, *to)) continue;
    network.world().after(network.delay(), [&network, to = *to, sender, frame, message] {
      if (const auto server = network.at(to); server && server->serving()) server->serve(sender, frame, message, {});
    });
  }
  return true;
}

inline size_t SimTransport::broadcast_notify(const net::Message& message, sched::FrameType frame) {
  size_t sent = 0;
  for (const auto& peer : std::vector<identity::NodeId>(_routes.begin(), _routes.end()))
    if (send(peer, net::Lane::control, message, frame)) ++sent;
  return sent;
}

inline void SimTransport::close_route(const identity::NodeId& peer, net::Lane, const std::string&) {
  _routes.erase(peer);
  if (const auto index = _network.index_of(peer)) _sent_as.erase(*index);
}

inline bool SimTransport::has_route(const identity::NodeId& peer, net::Lane) const {
  if (!_routes.contains(peer)) return false;
  const auto to = _network.index_of(peer);
  const auto server = to ? _network.at(*to) : nullptr;
  return server && server->serving();
}

// How a node comes back from a restart (Mimir's review): with everything; as the same node (its
// id kept, a new epoch) whose hosted store lost its state, the restart rule's case; or as a new
// node, a new identity in a new directory, a membership change.
enum class Wipe { nothing, store, identity };

// A cluster of whole nodes in the world: each node's state in its own directory, its clock at
// its own offset and drift, its loops on the world's schedule, its network the world's. A node
// stops and comes back with its state kept or wiped, as the test chooses (Mimir's second ask).
class SimNodes {
 public:
  // `size` nodes, each bootstrapping from node 0; clocks within `skew_ms` of the world's and
  // drifting up to `drift_ppm`, from the seed. Started at once, or by start_all() once a test has
  // set `configure` and `host` (which may read ids()).
  SimNodes(World& world, size_t size, NetworkFaults faults = {}, int64_t skew_ms = 0, int64_t drift_ppm = 0, bool started = true)
      : _world(world), _network(world, faults) {
    std::ofstream(_dir.path() / "cluster.key") << "sim cluster";
    _key = identity::load_cluster_key(_dir.path() / "cluster.key");
    for (size_t i = 0; i < size; ++i) {
      const int64_t offset = skew_ms ? static_cast<int64_t>(world.random.below(static_cast<uint64_t>(2 * skew_ms + 1))) - skew_ms : 0;
      const int64_t drift = drift_ppm ? static_cast<int64_t>(world.random.below(static_cast<uint64_t>(2 * drift_ppm + 1))) - drift_ppm : 0;
      _clocks.push_back(std::make_unique<NodeClock>(world, offset, drift));
      _nodes.emplace_back();
      _ids.push_back(draw_id());
    }
    if (started) start_all();
  }
  void start_all() {
    for (size_t i = 0; i < _nodes.size(); ++i) start(i);
  }
  ~SimNodes() {
    for (size_t i = 0; i < _nodes.size(); ++i) stop(i);
  }
  SimNodes(const SimNodes&) = delete;
  SimNodes& operator=(const SimNodes&) = delete;

  size_t size() const noexcept { return _nodes.size(); }
  bool up(size_t i) const { return _nodes.at(i) != nullptr; }
  Node& node(size_t i) { return *_nodes.at(i); }
  identity::NodeId id(size_t i) const { return _ids.at(i); }
  // Every node's id, known before any starts (a genesis set names them).
  const std::vector<identity::NodeId>& ids() const noexcept { return _ids; }
  SimNetwork& network() noexcept { return _network; }
  // Steps node `i`'s clocks forward by `by_ms` (NodeClock::step).
  void step_clock(size_t i, int64_t by_ms) { _clocks.at(i)->step(by_ms); }

  // What node `i` is configured with; a test may change it before a restart.
  NodeConfig config(size_t i) {
    NodeConfig out;
    out.state_path = _dir.path() / ("node" + std::to_string(i) + "-" + std::to_string(_generation[i]));
    out.key = _key;
    out.listen_host = "127.0.0.1";
    out.advertise_host = "127.0.0.1";
    out.port = SimNetwork::port_of(i);
    if (i != 0) out.bootstrap = {{"127.0.0.1", SimNetwork::port_of(0)}};
    out.inbound = reach::Inbound::yes;
    out.path_class = membership::PathClass::public_path;
    out.heartbeat = std::chrono::milliseconds(100);
    out.dead_after = std::chrono::milliseconds(2000);
    out.telemetry_interval = std::chrono::milliseconds(500);
    out.connect_timeout = std::chrono::milliseconds(1000);
    out.version = "0.9.0-sim";
    out.log = log;
    out.time = _clocks.at(i).get();
    out.wall = _clocks.at(i).get();
    out.schedule = [this](std::chrono::milliseconds after, std::function<void()> task) { _world.after(static_cast<uint64_t>(after.count()), std::move(task)); };
    out.transport = _network.transport_for(i);
    // The gates' CPU fixed, as the worker pool's is on a schedule: a real reading would make
    // each run its own.
    out.gate_process_cpu = [] { return 0.0; };
    // A start's fresh ids (its durability epoch, which seeds agreement's backoff, and telemetry's
    // boot id) from the seed, as the node ids are.
    out.fresh_id = [this] { return draw_id(); };
    if (configure) configure(i, out);
    return out;
  }
  // A test's own settings for every node it starts.
  std::function<void(size_t, NodeConfig&)> configure;
  // What the hosted system serves on node `i` once it has started, every start included:
  // `lost` says its store lost its state (Wipe::store or Wipe::identity), and must not vote
  // until caught up, unless it is new.
  std::function<void(size_t i, Node& node, bool lost)> host;
  // Every node's log, kept for a test to look through rather than printed.
  std::shared_ptr<RecordingSink> log = std::make_shared<RecordingSink>();

  void start(size_t i, bool lost = false) {
    _generation.resize(std::max(_generation.size(), i + 1), 0);
    auto settings = config(i);
    // The node's id from the seed, not the operating system's randomness: ids order the rosters
    // and every map keyed by them, so a run replays only if they do. Kept by a restart; a wiped
    // node's new directory draws a new one.
    if (!std::filesystem::exists(settings.state_path / "node-id")) {
      std::filesystem::create_directories(settings.state_path);
      std::ofstream(settings.state_path / "node-id") << identity::to_string(_ids.at(i)) << "\n";
    }
    auto node = std::make_unique<Node>(_world.io, std::move(settings));
    node->start();
    _nodes.at(i) = std::move(node);
    if (host) host(i, *_nodes.at(i), lost);
  }
  // Stopped, then destroyed once the world's ready handlers have run while it still exists:
  // after its stop nothing it booked on the schedule runs, and its transport, holding its hooks,
  // goes with it. A call it had in flight is never answered.
  void stop(size_t i) {
    if (!_nodes.at(i)) return;
    for (const auto& hook : stopping) hook(i);
    _nodes.at(i)->stop();
    _world.io.restart();
    while (_world.io.poll_one() > 0) {
    }
    _nodes.at(i).reset();
  }
  // Told before node `i` stops: what a test or a fixture holds of it ends with it (the stream
  // directory severs its streams).
  std::vector<std::function<void(size_t)>> stopping;

  // Back as Wipe says.
  void restart(size_t i, Wipe wipe = Wipe::nothing) {
    stop(i);
    if (wipe == Wipe::identity) {
      ++_generation.at(i);
      _ids.at(i) = draw_id();
    }
    start(i, wipe != Wipe::nothing);
  }

 private:
  identity::NodeId draw_id() {
    identity::NodeId id;
    for (auto& byte : id.bytes) byte = static_cast<uint8_t>(_world.random.next());
    return id;
  }

  World& _world;
  SimNetwork _network;
  TempDir _dir;
  identity::ClusterKey _key;
  std::vector<std::unique_ptr<NodeClock>> _clocks;
  std::vector<uint64_t> _generation;
  std::vector<identity::NodeId> _ids;
  std::vector<std::unique_ptr<Node>> _nodes;
};

}  // namespace dannan::test::sim
