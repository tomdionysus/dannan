//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/node.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <future>
#include <set>
#include <stdexcept>

#include "dannan/reach/external_ip.h"

namespace dannan {

namespace asio = boost::asio;
using membership::NodeRecord;

namespace {

NodeRecord initial_self(const NodeConfig& config, const identity::NodeId& id, bool capable) {
  NodeRecord self;
  self.id = id;
  self.host = config.advertise_host;
  self.port = config.port;
  self.failure_domain = config.failure_domain;
  self.capabilities = (capable ? membership::capability_inbound : 0U) | (config.relay ? membership::capability_relay : 0U);
  return self;
}

// What a topology event is for: where a node is, what it does, and what
// it advertises; its liveness and candidates are not.
bool shape_changed(const NodeRecord& before, const NodeRecord& after) {
  return before.host != after.host || before.port != after.port || before.failure_domain != after.failure_domain || before.capabilities != after.capabilities ||
         before.path_class != after.path_class || before.adverts != after.adverts;
}

std::vector<identity::NodeId> ids_of(const std::vector<NodeRecord>& nodes) {
  std::vector<identity::NodeId> out;
  for (const auto& node : nodes) out.push_back(node.id);
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

// --- The nouns: thin views of the node -------------------------------------

class Node::NodesNoun final : public Nodes {
 public:
  explicit NodesNoun(Node& node) : _node(node) {}
  NodeRecord self() const override { return _node._members.self(); }
  std::vector<NodeRecord> roster() const override { return _node._members.all(); }
  std::vector<NodeRecord> active() const override { return _node._members.active(); }
  bool directly_reachable(const identity::NodeId& node) const override { return _node._members.directly_reachable(node); }
  bool inbound_capable(const identity::NodeId& node) const override { return _node._members.inbound_capable(node); }
  RosterSnapshot snapshot() const override {
    RosterSnapshot out;
    out.all = _node._members.all();
    out.active = _node._members.active();
    return out;
  }
  bool all_known_reachable() const override { return _node._members.all_known_reachable(); }
  std::optional<net::Purpose> path(const identity::NodeId& node, net::Lane lane) const override {
    return _node._client ? _node._client->path_of(node, lane) : std::nullopt;
  }
  std::optional<std::chrono::milliseconds> latency(const identity::NodeId& node) const override {
    return _node._client ? _node._client->peer_latency(node) : std::nullopt;
  }
  std::map<identity::NodeId, std::chrono::milliseconds> latencies() const override {
    return _node._client ? _node._client->peer_latencies() : decltype(latencies()){};
  }
  std::vector<membership::IdentityReset> identity_resets() const override { return _node._members.identity_resets(); }
  bool apply_identity_reset(const membership::IdentityReset& reset) override { return _node.apply_identity_reset(reset); }
  void propagate_identity_reset(const membership::IdentityReset& reset, std::chrono::milliseconds deadline) override {
    _node.propagate_identity_reset(reset, deadline);
  }
  std::vector<NodeRecord> placement(std::span<const uint8_t> key, size_t copies, const placement::Scheme& scheme,
                                    const std::string& only_advertising) const override {
    auto candidates = _node._members.active();
    if (!only_advertising.empty()) candidates = placement::with_advert(candidates, only_advertising);
    return placement::by_capacity(key, candidates, copies, scheme);
  }
  void advertise(const std::string& key, net::Bytes value) override { _node._members.set_advert(key, std::move(value)); }
  void withdraw(const std::string& key) override { _node._members.clear_advert(key); }

 private:
  Node& _node;
};

class Node::RoutesNoun final : public Routes {
 public:
  explicit RoutesNoun(Node& node) : _node(node) {}
  void add(net::MessageType type, net::Registration registration, net::RequestHandler handler) override {
    if (_node._started) throw std::logic_error("a type's wire rules are fixed once the node has started");
    if (_node._table.is_core(type)) throw std::logic_error("message type " + std::to_string(static_cast<uint16_t>(type)) + " is one of Dannan's");
    _node._table.add(type, std::move(registration));
    if (handler) bind(type, std::move(handler));
  }
  void bind(net::MessageType type, net::RequestHandler handler) override {
    if (_node._table.is_core(type)) throw std::logic_error("message type " + std::to_string(static_cast<uint16_t>(type)) + " is one of Dannan's");
    contract::WriteLock lock(_node._handlers_mutex);
    _node._handlers[type] = std::move(handler);
  }
  void unbind(net::MessageType type) override {
    contract::WriteLock lock(_node._handlers_mutex);
    _node._handlers.erase(type);
  }
  std::shared_ptr<net::CallHandle> call(const NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                        std::chrono::milliseconds deadline, asio::any_io_executor executor, Done done) override {
    return _node.place_call(peer, {peer.host, peer.port}, type, std::move(payload), frame, deadline, std::move(executor), std::move(done));
  }
  std::shared_ptr<net::CallHandle> call(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                        std::chrono::milliseconds deadline, asio::any_io_executor executor, Done done) override {
    return _node.place_call(std::nullopt, endpoint, type, std::move(payload), frame, deadline, std::move(executor), std::move(done));
  }
  net::CallOutcome call_and_wait(const NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                 std::chrono::milliseconds deadline) override {
    return _node.call_wait(peer, {peer.host, peer.port}, type, std::move(payload), deadline, frame);
  }
  net::CallOutcome call_and_wait(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                 std::chrono::milliseconds deadline) override {
    return _node.call_wait(std::nullopt, endpoint, type, std::move(payload), deadline, frame);
  }

 private:
  Node& _node;
};

class Node::ChannelsNoun final : public Channels {
 public:
  explicit ChannelsNoun(Node& node) : _node(node) {}
  size_t broadcast(const net::Message& message) override { return usable() ? _node._client->broadcast(message) : 0; }
  size_t broadcast_best_effort(const net::Message& message, sched::FrameType frame) override {
    return usable() ? _node._client->broadcast_best_effort(message, frame) : 0;
  }
  size_t notify(const net::Message& message, sched::FrameType frame) override { return usable() ? _node._client->broadcast_notify(message, frame) : 0; }

 private:
  bool usable() const { return _node._client && !_node._stopping && !_node._calls_cancelled; }

 private:
  Node& _node;
};

class Node::IdentityNoun final : public Identity {
 public:
  explicit IdentityNoun(const Node& node) : _node(node) {}
  identity::NodeId id() const override { return _node._identity.id; }
  identity::NodeId durability_epoch() const override { return _node._identity.durability_epoch; }
  std::array<uint8_t, 32> derive_key(std::string_view label) const override { return _node._identity.key.derive(label); }

 private:
  const Node& _node;
};

class Node::BudgetNoun final : public Budget {
 public:
  explicit BudgetNoun(Node& node) : _node(node) {}
  void declare(sched::WorkClass work, uint64_t unit_bytes) override {
    const auto stats = _node._memory.stats();
    // What the class can count on: the whole, less the reassembly reserve
    // and what is held back for every other class.
    uint64_t held_back = stats.reassembly_reserve_bytes;
    if (work != sched::WorkClass::control) held_back += stats.control_reserve_bytes;
    if (work != sched::WorkClass::foreground) held_back += stats.foreground_reserve_bytes;
    if (work != sched::WorkClass::loader) held_back += stats.loader_reserve_bytes;
    const uint64_t room = stats.capacity_bytes > held_back ? stats.capacity_bytes - held_back : 0;
    if (unit_bytes > room)
      throw std::invalid_argument("a unit of " + std::string(sched::work_class_name(work)) + " work (" + std::to_string(unit_bytes) +
                                  " bytes) does not fit the node's budget for it (" + std::to_string(room) + " bytes)");
  }
  std::optional<sched::RetainedMemoryLedger::Lease> reserve(sched::WorkClass work, uint64_t bytes) override {
    return _node._memory.try_acquire(work, sched::MemoryOwner::hosted_first, bytes);
  }

 private:
  Node& _node;
};

class Node::FaultsNoun final : public Faults {
 public:
  explicit FaultsNoun(Node& node) : _node(node) {}
  void sink(Sink sink) override {
    contract::Lock lock(_mutex);
    _sink = std::move(sink);
  }
  void raise(std::string reason) override {
    _node._surface.counter("faults").fetch_add(1);
    _node._surface.publish("faults.last", reason);
    Sink sink;
    {
      contract::Lock lock(_mutex);
      sink = _sink;
    }
    if (sink) sink(std::move(reason));
  }

 private:
  Node& _node;
  contract::Mutex _mutex;
  Sink _sink DANNAN_GUARDED_BY(_mutex);
};

class Node::TimeNoun final : public Time {
 public:
  explicit TimeNoun(Node& node) : _node(node) {}
  contract::TimePoint now() const override { return _node._config.time->now(); }
  uint64_t unix_ms() const override { return _node._config.wall->unix_ms(); }
  void after(std::chrono::milliseconds delay, std::function<void()> fn) override {
    auto timer = std::make_shared<asio::steady_timer>(_node._io, delay);
    timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code& error) {
      if (!error) fn();
    });
  }

 private:
  Node& _node;
};

class Node::TelemetryNoun final : public Telemetry {
 public:
  explicit TelemetryNoun(Node& node) : _node(node) {}
  std::optional<status::Telemetry> local() const override { return _node._telemetry.local(); }
  std::vector<status::Telemetry> all() const override { return _node._telemetry.all(); }
  std::vector<status::TelemetryView> views(std::chrono::milliseconds fresh_for) const override { return _node._telemetry.views(fresh_for); }
  std::vector<status::Telemetry> persisted() const override { return _node._telemetry.persisted(); }
  void persist() override { _node._telemetry.persist(); }
  void refresh() override { _node.signal_telemetry(); }

 private:
  Node& _node;
};

Nodes& Node::nodes() { return *_nodes; }
const Nodes& Node::nodes() const { return *_nodes; }
Routes& Node::routes() { return *_routes_noun; }
Channels& Node::channels() { return *_channels; }
const Identity& Node::identity() const { return *_identity_noun; }
Budget& Node::budget() { return *_budget; }
Faults& Node::faults() { return *_faults; }
Time& Node::time() { return *_time; }
Telemetry& Node::telemetry() { return *_telemetry_noun; }

Diagnostics Node::diagnostics() const {
  Diagnostics out;
  out.started_unix_ms = _readiness.started_unix_ms();
  out.connectivity = connectivity();
  out.inbound = _inbound.state();
  if (_client) out.client = _client->stats();
  if (_server) out.work = _server->work_stats();
  out.traffic = traffic();
  out.threads = _threads.statuses();
  if (_paths) out.paths = _paths->stats();
  return out;
}

// --- The node ---------------------------------------------------------------

Node::Node(asio::io_context& io, NodeConfig config)
    : _io(io),
      _config(std::move(config)),
      _lock(_config.state_path),
      _identity(_config.state_path, _config.key),
      _inbound(_config.inbound, _config.state_path, _config.wall, _config.reprobe_while_capable, _config.reprobe_while_incapable, _config.heartbeat),
      _members(initial_self(_config, _identity.id, _inbound.capable()), _config.dead_after, _config.state_path / "membership" / "known-nodes.bin",
               _config.forget_after, _config.time, _config.wall),
      _telemetry(_identity.id, _config.state_path / "telemetry" / "last-known.bin", _config.time, _config.wall, _config.telemetry_layout),
      _readiness(_config.planes, _config.wall),
      _activity(_config.time),
      _memory(_config.memory_bytes, _config.control_reserve_bytes, _config.foreground_reserve_bytes, _config.loader_reserve_bytes,
              _config.reassembly_reserve_bytes),
      _nodes(std::make_unique<NodesNoun>(*this)),
      _routes_noun(std::make_unique<RoutesNoun>(*this)),
      _channels(std::make_unique<ChannelsNoun>(*this)),
      _identity_noun(std::make_unique<IdentityNoun>(*this)),
      _budget(std::make_unique<BudgetNoun>(*this)),
      _faults(std::make_unique<FaultsNoun>(*this)),
      _time(std::make_unique<TimeNoun>(*this)),
      _telemetry_noun(std::make_unique<TelemetryNoun>(*this)) {
  // Tombstones loaded with the roster fence telemetry before anything runs.
  for (const auto& reset : _members.identity_resets()) _telemetry.apply_identity_reset(reset);
}

Node::~Node() { stop(); }

void Node::start() {
  // A cluster nobody could connect to is refused before anything listens.
  reach::refuse_impossible_cluster(_config.inbound, _config.bootstrap, _members.all(), _identity.id);
  if (_started.exchange(true)) return;
  _frozen = std::make_shared<const net::RouteTable>(_table);

  net::ServerOptions server_options;
  server_options.key = _identity.key;
  server_options.local = [this] { return _members.self(); };
  server_options.max_frame = _config.max_frame;
  server_options.routes = _frozen;
  server_options.limits = _config.limits;
  server_options.memory = &_memory;
  server_options.traffic = _traffic;
  server_options.time = _config.time;
  _server = std::make_unique<net::Server>(
      _io, _config.listen_host, _config.port, server_options,
      [this](const NodeRecord& peer, sched::FrameType frame, const net::Message& request) { return handle(peer, frame, request); },
      [this](const std::shared_ptr<net::Session>& session) {
        if (_client) _client->admit(session);
        observe_direct(session->peer());
      });

  net::ClientOptions client_options;
  client_options.key = _identity.key;
  client_options.local = [this] { return _members.self(); };
  client_options.max_frame = _config.max_frame;
  client_options.routes = _frozen;
  client_options.memory = &_memory;
  client_options.traffic = _traffic;
  client_options.time = _config.time;
  client_options.connect_timeout = _config.connect_timeout;
  client_options.control_no_progress = _config.control_no_progress;
  client_options.data_no_progress = _config.data_no_progress;
  client_options.heartbeat = _config.heartbeat;
  client_options.dead_after = _config.dead_after;
  client_options.alive = [this](const NodeRecord& peer) { observe_direct(peer); };
  client_options.established = [this](const NodeRecord& peer) { observe_direct(peer); };
  client_options.inbound.request = [this](const NodeRecord& peer, net::Frame frame, net::Reply reply) {
    _server->scheduler().submit(peer, std::move(frame), std::move(reply));
  };
  client_options.inbound.promoted = [this](const identity::NodeId& peer, uint64_t id, sched::FrameType frame) {
    _server->scheduler().promote(peer, id, frame);
  };
  client_options.inbound.cancelled = [this](const identity::NodeId& peer, uint64_t id) { _server->scheduler().cancel(peer, id); };
  client_options.make_path = [this](const identity::NodeId& peer, net::Lane lane, std::function<void(std::string)> failed) {
    if (_paths)
      _paths->make_path(peer, lane, std::move(failed));
    else
      failed("level 0 is not running");
  };
  _client = std::make_unique<net::Client>(_io, client_options);
  reach::PathsConfig paths;
  paths.key = _identity.key;
  paths.connect_timeout = _config.connect_timeout;
  paths.punch_window = _config.punch_window;
  paths.relay = _config.relay;
  paths.forced_class = _config.path_class;
  _paths = std::make_unique<reach::Paths>(
      _io, paths, *_client, [this] { return _members.self(); }, [this] { return _members.active(); },
      [this](const NodeRecord& peer, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline) {
        return call_wait(peer, {peer.host, peer.port}, type, std::move(payload), deadline);
      });
  if (_config.path_class) _members.set_path_class(*_config.path_class);
  // An inbound-incapable node keeps both lanes dialled to every capable
  // peer: only it can restore its own reachability.
  _client->set_maintained_peers([this] {
    std::vector<NodeRecord> out;
    for (auto& node : _members.active())
      if (node.id != _identity.id && node.inbound_capable()) out.push_back(std::move(node));
    return out;
  });
  // The transport must know a peer that cannot be dialled before its first
  // exchange, not after a refused dial.
  for (const auto& node : _members.all())
    if (node.id != _identity.id) _client->note_peer(node);
  for (const auto& reset : _members.identity_resets()) _client->invalidate_identity_association(reset);

  _dial_back = std::make_unique<reach::DialBackResponder>(
      _config.dial_back_min_interval,
      [this](const net::Endpoint& endpoint, const identity::NodeId& expected) {
        auto done = std::make_shared<std::promise<std::string>>();
        auto future = done->get_future();
        _client->probe(endpoint, expected, _io.get_executor(), [done](std::string error) { done->set_value(std::move(error)); });
        if (future.wait_for(_config.connect_timeout * 2) != std::future_status::ready) return std::string("dial-back probe timed out");
        return future.get();
      },
      _config.time);

  _server->start();
  _bound_port = _server->bound_port();
  const auto self = _members.self();
  if (!self.host.empty() && self.port != _bound_port) _members.set_endpoint(self.host, _bound_port);
  _connectivity =
      std::make_unique<reach::PublicConnectivity>(_config.connectivity, _identity.id, net::Endpoint{_members.self().host, _bound_port}, _config.gateway,
                                                  _config.external_ip ? _config.external_ip : reach::ExternalIpLookup(reach::checkip_external_ip),
                                                  _config.self_probe ? _config.self_probe : reach::SelfProbe(reach::tcp_self_probe), _config.wall);
  _readiness.control_plane(true);
  _client->start();

  _loops.emplace_back([this](std::stop_token stop) { _threads.loop("node-membership", stop, [this, stop] { membership_loop(stop); }); });
  _loops.emplace_back([this](std::stop_token stop) { _threads.loop("node-telemetry", stop, [this, stop] { telemetry_loop(stop); }); });
  _loops.emplace_back([this](std::stop_token stop) { _threads.loop("node-connectivity", stop, [this, stop] { connectivity_loop(stop); }); });
}

void Node::cancel_outbound_calls() {
  if (!_started || _calls_cancelled.exchange(true)) return;
  if (_client) _client->stop();
}

void Node::stop() {
  if (!_started || _stopping.exchange(true)) return;
  for (auto& loop : _loops) loop.request_stop();
  _wake.notify_all();
  // The transport goes first: a loop waiting on a call is released by it.
  if (_server) _server->stop();
  cancel_outbound_calls();
  if (_paths) _paths->stop();
  for (auto& loop : _loops)
    if (loop.joinable()) loop.join();
  _loops.clear();
  _readiness.control_plane(false);
  // Last: the mapping this node made.
  _connectivity.reset();
}

std::optional<reach::PublicConnectivityStatus> Node::connectivity() const {
  if (!_connectivity) return std::nullopt;
  return _connectivity->status();
}

std::optional<net::ClientStats> Node::client_stats() const {
  if (!_client) return std::nullopt;
  return _client->stats();
}

std::optional<net::WorkStats> Node::work_stats() const {
  if (!_server) return std::nullopt;
  return _server->work_stats();
}

status::TrafficTotals Node::traffic() const {
  status::TrafficTotals out;
  for (size_t i = 0; i < out.in_bytes.size(); ++i) {
    out.in_bytes[i] = _traffic->in_bytes[i].load(std::memory_order_relaxed);
    out.out_bytes[i] = _traffic->out_bytes[i].load(std::memory_order_relaxed);
  }
  return out;
}

void Node::signal_telemetry() {
  _telemetry_demand.fetch_add(1, std::memory_order_release);
  _wake.notify_all();
}

net::Handled Node::handle(const NodeRecord& peer, sched::FrameType frame, const net::Message& request) {
  namespace m = net::message;
  const auto type = request.type;
  _activity.note(frame, request.payload.size());
  if (type == m::ping) return net::Handled::processed();
  if (type == m::members) {
    const auto nodes = _members.all();
    net::Writer writer;
    writer.u32(static_cast<uint32_t>(nodes.size()));
    for (const auto& node : nodes) membership::encode(writer, node);
    return net::Handled::processed({m::members_reply, writer.take(), {}});
  }
  if (type == m::telemetry) {
    if (!request.payload.empty()) {
      try {
        for (auto& record : status::decode_telemetry_set(request.payload)) _telemetry.observe(std::move(record));
      } catch (const net::DecodeError&) {
        // A single record, sent as a request.
        _telemetry.observe(status::decode_telemetry(request.payload), true);
      }
    }
    const auto ttl = std::max(_config.dead_after * 2, std::chrono::milliseconds(60000));
    return net::Handled::processed({m::telemetry_reply, status::encode_telemetry_set(_telemetry.recent(ttl, 64)), {}});
  }
  if (type == m::identity_resets) {
    if (!request.payload.empty())
      for (const auto& reset : membership::decode_identity_resets(request.payload)) apply_identity_reset(reset);
    return net::Handled::processed({m::identity_resets_reply, membership::encode_identity_resets(_members.identity_resets()), {}});
  }
  if (type == m::dial_request) return _client->handle_dial_request(peer, request);
  if (type == m::punch_request) return _paths->punch_request(peer, request);
  if (type == m::punch_go) return _paths->punch_go(peer, request);
  if (type == m::relay_open) return _paths->relay_open(peer, request);
  if (type == m::relay_offer) return _paths->relay_offer(peer, request);
  if (type == m::relay_data) return _paths->relay_data(peer, request);
  if (type == m::relay_close) return _paths->relay_close(peer, request);
  if (type == m::dial_back) return _dial_back->answer(peer.id, request.payload);
  // Held shared across the call: unbind waits for it.
  contract::ReadLock lock(_handlers_mutex);
  const auto handler = _handlers.find(type);
  if (handler == _handlers.end() || !handler->second) return net::Handled::unrecognised();
  return handler->second(peer, frame, request);
}

void Node::observe_direct(const NodeRecord& peer) {
  const auto before = _members.active();
  const auto previous = std::find_if(before.begin(), before.end(), [&](const NodeRecord& n) { return n.id == peer.id; });
  const bool changed = previous == before.end() || shape_changed(*previous, peer);
  _members.observe(peer, true);
  if (_client && peer.id != _identity.id) _client->note_peer(peer);
  if (changed) _events.notify(status::NodeEvents::topology);
  signal_telemetry();
}

void Node::merge(std::span<const uint8_t> payload) {
  const auto before_all = _members.all();
  const auto before_active = _members.active();
  bool changed = false;
  net::Reader reader(payload);
  const auto count = reader.u32();
  if (count > 100000) throw net::DecodeError("member list too large");
  for (uint32_t i = 0; i < count; ++i) {
    auto node = membership::decode(reader);
    const auto previous = std::find_if(before_all.begin(), before_all.end(), [&](const NodeRecord& n) { return n.id == node.id; });
    changed = changed || previous == before_all.end() || shape_changed(*previous, node);
    if (node.id != _identity.id && _client) _client->note_peer(node);
    _members.observe(std::move(node));
  }
  reader.finish();
  // An exchange is a heartbeat: identical gossip wakes nobody.
  if (changed || ids_of(before_active) != ids_of(_members.active())) _events.notify(status::NodeEvents::topology);
}

std::shared_ptr<net::CallHandle> Node::place_call(std::optional<NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload,
                                                  sched::FrameType frame, std::chrono::milliseconds deadline, asio::any_io_executor executor,
                                                  net::Client::Done done) {
  if (!_started || _stopping || _calls_cancelled || !_client) {
    asio::post(executor, [done = std::move(done), why = std::string(_started ? "node is stopping; outbound calls are cancelled" : "node is not started")] {
      done(net::CallOutcome{std::nullopt, why, {}});
    });
    return std::make_shared<net::CallHandle>();
  }
  // Whichever comes first, the answer or the deadline, is the one told.
  struct State {
    std::atomic<bool> finished{false};
    net::Client::Done done;
  };
  auto state = std::make_shared<State>();
  state->done = std::move(done);
  auto strand = asio::make_strand(_io);
  auto timer = std::make_shared<asio::steady_timer>(strand, deadline);
  timer->async_wait([state, executor, deadline](const boost::system::error_code& error) {
    if (error || state->finished.exchange(true)) return;
    asio::post(executor, [state, deadline] {
      state->done(net::CallOutcome{std::nullopt, "deadline of " + std::to_string(deadline.count()) + " ms passed before an answer", {}});
    });
  });
  auto finish = [state, timer, strand](net::CallOutcome outcome) {
    asio::post(strand, [timer] { timer->cancel(); });
    if (state->finished.exchange(true)) return;
    state->done(std::move(outcome));
  };
  if (peer) return _client->call(*peer, type, std::move(payload), frame, executor, std::move(finish));
  return _client->call(endpoint, type, std::move(payload), frame, executor, std::move(finish));
}

net::CallOutcome Node::call_wait(std::optional<NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload,
                                 std::chrono::milliseconds deadline, sched::FrameType frame) {
  auto done = std::make_shared<std::promise<net::CallOutcome>>();
  auto future = done->get_future();
  place_call(std::move(peer), endpoint, type, std::move(payload), frame, deadline, _io.get_executor(),
             [done](net::CallOutcome outcome) { done->set_value(std::move(outcome)); });
  // The deadline answers first; the margin only covers the executor.
  if (future.wait_for(deadline + std::chrono::seconds(5)) != std::future_status::ready) return {std::nullopt, "no answer", {}};
  return future.get();
}

void Node::exchange(std::optional<NodeRecord> peer, const net::Endpoint& endpoint) {
  const auto outcome = call_wait(std::move(peer), endpoint, net::message::members, {}, _config.connect_timeout * 2);
  if (!outcome.reply) throw std::runtime_error(outcome.failure);
  if (outcome.reply->type != net::message::members_reply) throw std::runtime_error("membership refused");
  merge(outcome.reply->payload);
}

void Node::sleep_until(std::stop_token stop, contract::TimePoint due) {
  std::unique_lock lock(_wait_mutex);
  const auto now = _config.time->now();
  if (due > now) _wake.wait_for(lock, stop, due - now, [] { return false; });
}

void Node::membership_loop(std::stop_token stop) {
  while (!stop.stop_requested()) {
    if (_members.forget_unseen()) _events.notify(status::NodeEvents::topology);
    const auto known = _members.all();
    _peers_known = static_cast<uint32_t>(known.size());
    uint32_t active = 1;
    std::set<std::pair<std::string, uint16_t>> exchanged;
    // A peer that takes no inbound connection is exchanged with only over
    // its own session; without one there is nothing to dial.
    const auto unreachable = [&](const NodeRecord& node) { return !node.inbound_capable() && !_client->has_route(node.id, net::Lane::control); };
    for (const auto& endpoint : _config.bootstrap) {
      if (stop.stop_requested()) return;
      exchanged.emplace(endpoint.host, endpoint.port);
      const auto found = std::find_if(known.begin(), known.end(), [&](const NodeRecord& node) {
        return node.id != _identity.id && node.host == endpoint.host && node.port == endpoint.port;
      });
      try {
        if (found != known.end() && unreachable(*found)) continue;
        exchange(found != known.end() ? std::optional(*found) : std::nullopt, endpoint);
        ++active;
      } catch (const std::exception&) {
        // A seed down is ordinary; the next heartbeat tries again.
      }
    }
    for (const auto& node : known) {
      if (stop.stop_requested()) return;
      if (node.id == _identity.id || !exchanged.emplace(node.host, node.port).second || unreachable(node)) continue;
      try {
        exchange(node, {node.host, node.port});
        ++active;
      } catch (const std::exception&) {
      }
    }
    _peers_active = active;
    sleep_until(stop, _config.time->now() + _config.heartbeat);
  }
}

void Node::telemetry_loop(std::stop_token stop) {
  const auto interval = std::max(_config.telemetry_interval, std::chrono::milliseconds(250));
  // Sampling follows demand; gossip at most once a second.
  const auto gossip_every = std::min(interval, std::chrono::milliseconds(1000));
  const auto ttl = std::max(_config.dead_after * 2, std::chrono::milliseconds(60000));
  contract::TimePoint last_gossip{};
  while (!stop.stop_requested()) {
    const auto demand = _telemetry_demand.load(std::memory_order_acquire);
    try {
      status::LocalFigures figures;
      figures.self = _members.self();
      figures.version = _config.version;
      figures.phase = _readiness.phase();
      figures.peers_known = _peers_known;
      figures.peers_active = _peers_active;
      figures.connections_canonical = _peers_active > 0 ? _peers_active - 1 : 0;
      figures.node_name = _config.node_name;
      figures.traffic = traffic();
      _telemetry.refresh_local(figures, _config.telemetry_fields);
      if (const auto now = _config.time->now(); now - last_gossip >= gossip_every) {
        last_gossip = now;
        const auto records = _telemetry.recent(ttl, 64);
        // A node stays visible while busy: speculative, never waiting.
        if (!records.empty())
          (void)_client->broadcast_best_effort({net::message::telemetry, status::encode_telemetry_set(records), {}}, sched::FrameType::speculative);
      }
    } catch (const std::exception&) {
      // The next tick samples again.
    }
    std::unique_lock lock(_wait_mutex);
    _wake.wait_for(lock, stop, interval, [&] { return _telemetry_demand.load(std::memory_order_acquire) != demand; });
  }
}

void Node::apply_inbound(bool capable) {
  if (_members.set_inbound_capable(capable)) {
    _events.notify(status::NodeEvents::topology);
    signal_telemetry();
  }
}

std::chrono::milliseconds Node::classify() {
  const auto self = _members.self();
  bool changed = false;
  auto again = _config.heartbeat;
  if (_config.path_class) {
    changed = _members.set_path_class(*_config.path_class);
  } else if (self.inbound_capable()) {
    // Dialable where it advertises: that is its one candidate, confirmed by
    // the dial-back or the operator.
    changed = _members.set_path_class(membership::PathClass::public_path);
    const bool same = self.candidates.size() == 1 && self.candidates[0].host == self.host && self.candidates[0].port == self.port;
    if (!same) changed = _members.set_candidates({{self.host, self.port, true, _config.wall->unix_ms()}}) || changed;
  } else if (const auto observation = _paths->observe()) {
    changed = _members.set_path_class(reach::classify(*observation));
    std::vector<membership::Candidate> candidates;
    for (const auto& seen : observation->seen) {
      const auto known = std::any_of(candidates.begin(), candidates.end(), [&](const auto& c) { return c.host == seen.host && c.port == seen.port; });
      if (!known && candidates.size() < membership::max_candidates) candidates.push_back({seen.host, seen.port, false, 0});
    }
    changed = _members.set_candidates(std::move(candidates)) || changed;
    again = _config.reprobe_while_incapable;
  }
  if (changed) {
    _events.notify(status::NodeEvents::topology);
    signal_telemetry();
  }
  return again;
}

void Node::connectivity_loop(std::stop_token stop) {
  const auto before = _members.self();
  const auto status = _connectivity->refresh(_config.connectivity.check.enabled);
  if (!status.advertised.host.empty() && status.advertised.port && (status.advertised.host != before.host || status.advertised.port != before.port))
    _members.set_endpoint(status.advertised.host, status.advertised.port);
  const bool automatic = _config.inbound == reach::Inbound::automatic;
  auto next = _config.time->now();
  auto next_class = next;
  while (!stop.stop_requested()) {
    sleep_until(stop, automatic ? std::min(next, next_class) : next_class);
    if (stop.stop_requested()) return;
    // Level 0: this node's path class and candidates, and the circuits
    // through it that have lost an end.
    if (_config.time->now() >= next_class) next_class = _config.time->now() + classify();
    _paths->sweep();
    if (!automatic || _config.time->now() < next) continue;
    // Evidence is a peer, reached over CONTROL, reporting whether a fresh
    // connection to the advertised endpoint completed a handshake. An
    // unaskable peer is no evidence.
    next = _config.time->now() + _config.heartbeat;
    std::optional<NodeRecord> peer;
    for (const auto& node : _members.active()) {
      if (node.id != _identity.id && node.inbound_capable() && _client->has_route(node.id, net::Lane::control)) {
        peer = node;
        break;
      }
    }
    if (!peer) continue;
    const auto self = _members.self();
    const auto outcome = call_wait(peer, {peer->host, peer->port}, net::message::dial_back, reach::encode_dial_back_request({self.host, self.port}),
                                   _config.connect_timeout * 3);
    if (!outcome.reply || outcome.reply->type != net::message::dial_back_reply) continue;
    try {
      const auto [reachable, error] = reach::decode_dial_back_reply(outcome.reply->payload);
      const auto verdict = _inbound.observe_probe(peer->id, reachable, error);
      if (verdict.changed) apply_inbound(_inbound.capable());
      next = _config.time->now() + verdict.next_probe;
    } catch (const net::DecodeError&) {
    }
  }
}

bool Node::apply_identity_reset(const membership::IdentityReset& reset) {
  const bool changed = _members.apply_identity_reset(reset);
  // Idempotent: align every consumer even if one learned it first.
  _telemetry.apply_identity_reset(reset);
  if (_client) _client->invalidate_identity_association(reset);
  if (changed) _events.notify(status::NodeEvents::topology);
  return changed;
}

void Node::propagate_identity_reset(const membership::IdentityReset& reset, std::chrono::milliseconds deadline) {
  (void)apply_identity_reset(reset);
  const auto payload = membership::encode_identity_resets({reset});
  for (const auto& peer : _members.active()) {
    if (peer.id == _identity.id) continue;
    const auto outcome = call_wait(peer, {peer.host, peer.port}, net::message::identity_resets, payload, deadline);
    if (!outcome.reply || outcome.reply->type != net::message::identity_resets_reply) continue;
    try {
      for (const auto& learned : membership::decode_identity_resets(outcome.reply->payload)) apply_identity_reset(learned);
    } catch (const net::DecodeError&) {
    }
  }
}

}  // namespace dannan
