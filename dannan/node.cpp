//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/node.h"

#include <unistd.h>

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

// Runs `fn` once `time` reaches `at`, on the timer's executor, unless the timer
// is cancelled first. In the node's injected time, so a test can move it.
void when_time_reaches(const std::shared_ptr<asio::steady_timer>& timer, const contract::TimeSource& time, contract::TimePoint at, std::function<void()> fn) {
  timer->expires_after(time.wake_within(std::max(std::chrono::ceil<std::chrono::milliseconds>(at - time.now()), std::chrono::milliseconds(0))));
  timer->async_wait([timer, &time, at, fn = std::move(fn)](const boost::system::error_code& error) mutable {
    if (error) return;
    if (time.now() < at) return when_time_reaches(timer, time, at, std::move(fn));
    fn();
  });
}

NodeRecord initial_self(const NodeConfig& config, const identity::NodeId& id, const identity::NodeId& incarnation, bool capable) {
  NodeRecord self;
  self.id = id;
  self.host = config.advertise_host;
  if (self.host.empty()) {
    if (config.listen_host == "127.0.0.1" || config.listen_host == "::1") {
      self.host = config.listen_host;
    } else {
      char hostname[256]{};
      if (::gethostname(hostname, 255) == 0) self.host = hostname;
      if (self.host.empty()) self.host = "127.0.0.1";
    }
  }
  self.port = config.port;
  self.failure_domain = config.failure_domain.empty() ? self.host : config.failure_domain;
  self.incarnation = incarnation;
  self.capabilities = (capable ? membership::capability_inbound : 0U) | (config.relay ? membership::capability_relay : 0U);
  return self;
}

// What a membership event is for: where a node is, what it does, and how
// it is reached; its liveness and candidates are not.
bool place_changed(const NodeRecord& before, const NodeRecord& after) {
  return before.host != after.host || before.port != after.port || before.failure_domain != after.failure_domain || before.incarnation != after.incarnation ||
         before.capabilities != after.capabilities || before.path_class != after.path_class;
}

// What a topology event is for: the same, and what the node advertises.
bool shape_changed(const NodeRecord& before, const NodeRecord& after) { return place_changed(before, after) || before.adverts != after.adverts; }

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
  std::optional<contract::TimePoint> absent_since(const identity::NodeId& node) const override { return _node._members.absent_since(node); }
  std::optional<identity::Certificate> certificate(const identity::NodeId& node) const override { return _node.peer_certificate(node); }
  bool directly_reachable(const identity::NodeId& node) const override { return _node._members.directly_reachable(node); }
  bool inbound_capable(const identity::NodeId& node) const override { return _node._members.inbound_capable(node); }
  RosterSnapshot snapshot() const override {
    auto [all, active] = _node._members.all_and_active();
    return {std::move(all), std::move(active)};
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
  void refresh_public_connectivity(bool probe, bool force_probe, asio::any_io_executor executor,
                                   std::function<void(std::optional<reach::PublicConnectivityStatus>)> done) override {
    _node.refresh_public_connectivity(probe, force_probe, std::move(executor), std::move(done));
  }
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
  uint64_t watch(asio::any_io_executor executor, std::function<void(RosterChange)> handler) override {
    contract::Lock lock(_node._watch_mutex);
    const auto id = _node._next_watcher++;
    // Told of the roster as the others were last told of it, so every
    // watcher sees the same sequence from here.
    for (const auto& [_, node] : _node._told) asio::post(executor, [handler, node] { handler({RosterChange::Kind::joined, node}); });
    _node._watchers.emplace(id, Watcher{std::move(executor), std::move(handler)});
    return id;
  }
  void unwatch(uint64_t id) override {
    contract::Lock lock(_node._watch_mutex);
    _node._watchers.erase(id);
  }
  void withdraw(const std::string& key) override { _node._members.clear_advert(key); }
  membership::Partition partition() const override {
    contract::Lock lock(_node._watch_mutex);
    return _node.partition_locked();
  }
  uint64_t watch_partition(asio::any_io_executor executor, std::function<void(membership::Partition)> handler) override {
    contract::Lock lock(_node._watch_mutex);
    const auto id = _node._next_watcher++;
    asio::post(executor, [handler, now = _node.partition_locked()] { handler(now); });
    _node._partition_watchers.emplace(id, PartitionWatcher{std::move(executor), std::move(handler)});
    return id;
  }
  void unwatch_partition(uint64_t id) override {
    contract::Lock lock(_node._watch_mutex);
    _node._partition_watchers.erase(id);
  }
  std::set<identity::NodeId> members() const override { return _node._agreed.members(); }
  bool retire(const identity::NodeId& node) override {
    const bool retired = _node._agreed.retire(node);
    if (retired) _node.topology_changed(true);
    return retired;
  }

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
  void retire(net::MessageType type) override {
    if (_node._started) throw std::logic_error("message types are retired before the node starts");
    _node._table.retire(type);
  }
  void bind(net::MessageType type, net::RequestHandler handler) override {
    if (!handler) return bind_deferred(type, {});
    bind_deferred(type, [handler = std::move(handler)](const NodeRecord& peer, sched::FrameType frame, net::Message request, net::Answer&) {
      return handler(peer, frame, request);
    });
  }
  void bind_deferred(net::MessageType type, net::DeferringHandler handler) override {
    if (_node._table.is_core(type)) throw std::logic_error("message type " + std::to_string(static_cast<uint16_t>(type)) + " is one of Dannan's");
    contract::WriteLock lock(_node._handlers_mutex);
    _node._handlers[type] = std::move(handler);
  }
  void unbind(net::MessageType type) override {
    contract::WriteLock lock(_node._handlers_mutex);
    _node._handlers.erase(type);
  }
  std::shared_ptr<net::CallHandle> call(const NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                        std::chrono::milliseconds deadline, asio::any_io_executor executor, Done done, net::CallWatch watch) override {
    return _node.place_call(peer, {peer.host, peer.port}, type, std::move(payload), frame, deadline, std::move(executor), std::move(done), watch);
  }
  std::shared_ptr<net::CallHandle> call(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                        std::chrono::milliseconds deadline, asio::any_io_executor executor, Done done, net::CallWatch watch) override {
    return _node.place_call(std::nullopt, endpoint, type, std::move(payload), frame, deadline, std::move(executor), std::move(done), watch);
  }
  net::CallOutcome call_and_wait(const NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                                 net::CallWatch watch) override {
    return _node.call_wait(peer, {peer.host, peer.port}, type, std::move(payload), deadline, frame, watch);
  }
  net::CallOutcome call_and_wait(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                 std::chrono::milliseconds deadline, net::CallWatch watch) override {
    return _node.call_wait(std::nullopt, endpoint, type, std::move(payload), deadline, frame, watch);
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
  void open(std::string name, net::Delivery delivery, size_t replay_bytes) override { _node._fanout->open(std::move(name), delivery, replay_bytes); }
  uint64_t subscribe(const std::string& channel, std::string prefix, asio::any_io_executor executor, net::ChannelHandler handler) override {
    return _node._fanout->subscribe(channel, std::move(prefix), std::move(executor), std::move(handler));
  }
  void unsubscribe(uint64_t id) override { _node._fanout->unsubscribe(id); }
  net::Published publish(const std::string& channel, std::string topic, net::Bytes payload) override {
    return _node._fanout->publish(channel, std::move(topic), std::move(payload));
  }
  void publish_bulk(const std::string& channel, std::string topic, net::Bytes payload, std::chrono::milliseconds deadline, asio::any_io_executor executor,
                    std::function<void(net::Published)> done) override {
    _node._fanout->publish_bulk(channel, std::move(topic), std::move(payload), deadline, std::move(executor), std::move(done));
  }

 private:
  bool usable() const { return _node._client && !_node._stopping && !_node._calls_cancelled; }

 private:
  Node& _node;
};

class Node::IdentityNoun final : public Identity {
 public:
  explicit IdentityNoun(Node& node) : _node(node) {}
  identity::NodeId id() const override { return _node._identity.id; }
  identity::NodeId durability_epoch() const override { return _node._identity.durability_epoch; }
  identity::Secret derive_key(std::string_view label) const override {
    auto key = _node._identity.key.derive(label);
    identity::Secret out(key);
    identity::cleanse(key);
    return out;
  }
  bool has_custody() const override { return _node._custody.has_value(); }
  std::optional<identity::Certificate> certificate() const override { return _node._config.certificate; }
  std::optional<identity::Certificate> ca_certificate() const override { return _node._config.ca_certificate; }
  void revoke(const identity::Revocation& revocation) override {
    if (!_node._revocations) throw std::logic_error("this node holds no certificates");
    if (!identity::verify_revocation(revocation, *_node._config.ca_certificate)) throw std::invalid_argument("the revocation is not this cluster's CA's");
    (void)_node.apply_revocations({revocation});
    for (const auto& peer : _node._members.active())
      if (peer.id != _node._identity.id) _node.exchange_revocations(peer, {peer.host, peer.port}, _node._config.connect_timeout);
  }
  void install_scope_source(std::shared_ptr<const contract::ScopeSource> source) override {
    contract::Lock lock(_node._scope_mutex);
    _node._scope = std::move(source);
  }
  contract::ScopeDecision verify(const contract::Principal& principal, std::string_view prefix) const override {
    std::shared_ptr<const contract::ScopeSource> source;
    {
      contract::Lock lock(_node._scope_mutex);
      source = _node._scope;
    }
    if (source) return source->may_write(principal, prefix);
    // No directory: the cluster's own nodes, as a cluster key gives them
    // today, and nobody else.
    if (principal.kind != contract::Principal::Kind::node) return {false, 0, "no scope source is installed, and only the cluster's nodes may write"};
    const auto id = identity::NodeId::parse(principal.id);
    if (!id) return {false, 0, "not a node id"};
    if (*id == _node._identity.id) return {true, 0, {}};
    for (const auto& node : _node._members.all())
      if (node.id == *id) return {true, 0, {}};
    return {false, 0, "not a node of this cluster"};
  }
  std::vector<identity::Revocation> revocations() const override { return _node._revocations ? _node._revocations->all() : decltype(revocations()){}; }
  identity::Bytes node_public_key() const override { return _node._custody ? _node._custody->node_key().public_key() : identity::Bytes{}; }
  identity::Bytes sign(std::span<const uint8_t> message) const override { return custody().node_key().sign(message); }
  identity::Secret derive_local(std::string_view label) const override { return custody().derive(label); }

 private:
  const identity::KeyCustody& custody() const {
    if (!_node._custody) throw std::logic_error("this node has no key custody (NodeConfig::custody)");
    return *_node._custody;
  }

 public:
 private:
  Node& _node;
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
  const contract::TimeSource& source() const override { return *_node._config.time; }
  asio::any_io_executor executor() const override { return _node._io.get_executor(); }
  void after(std::chrono::milliseconds delay, std::function<void()> fn) override {
    auto timer = std::make_shared<asio::steady_timer>(_node._io);
    when_time_reaches(timer, *_node._config.time, _node._config.time->now() + delay, std::move(fn));
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
Identity& Node::identity() { return *_identity_noun; }
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

void fit_memory_limit(NodeConfig& config, std::vector<sched::MemoryPart>& hosted) {
  config.memory_fitted = true;
  if (!config.memory_limit_bytes) return;
  sched::MemoryPlan plan;
  plan.parts = hosted;
  plan.parts.push_back({"runtime.retained_memory_bytes", config.memory_bytes});
  plan.reserves = {{"control", config.control_reserve_bytes},
                   {"foreground", config.foreground_reserve_bytes},
                   {"loader", config.loader_reserve_bytes},
                   {"reassembly", config.reassembly_reserve_bytes}};
  config.memory_warning = sched::fit_to_limit(config.memory_limit_bytes, plan);
  for (size_t i = 0; i < hosted.size(); ++i) hosted[i].bytes = plan.parts[i].bytes;
  config.memory_bytes = plan.parts.back().bytes;
  config.control_reserve_bytes = plan.reserves[0].bytes;
  config.foreground_reserve_bytes = plan.reserves[1].bytes;
  config.loader_reserve_bytes = plan.reserves[2].bytes;
  config.reassembly_reserve_bytes = plan.reserves[3].bytes;
}

namespace {

// Before any member is built from it.
NodeConfig prepared(NodeConfig config) {
  if (!config.log) config.log = std::make_shared<status::ConsoleSink>();
  if (!config.memory_fitted) {
    std::vector<sched::MemoryPart> none;
    fit_memory_limit(config, none);
  }
  return config;
}

}  // namespace

void Node::IoCpu::tick() {
  std::lock_guard lock(mutex);
  auto [found, fresh] = reporters.try_emplace(std::this_thread::get_id(), "net-io", std::chrono::seconds(5), status::ScopedLogger(log, "net"));
  (void)fresh;
  found->second.tick();
}

// --- The node ---------------------------------------------------------------

Node::Node(asio::io_context& io, NodeConfig config)
    : _io(io),
      _config(prepared(std::move(config))),
      _lock(_config.state_path),
      _identity(_config.state_path, _config.key),
      _custody(_config.custody ? std::optional<identity::KeyCustody>(identity::KeyCustody::open(_config.state_path, *_config.custody)) : std::nullopt),
      _inbound(_config.inbound, _config.state_path, _config.wall, _config.reprobe_while_capable, _config.reprobe_while_incapable, _config.heartbeat),
      _members(initial_self(_config, _identity.id, _identity.durability_epoch, _inbound.capable()), _config.dead_after,
               _config.state_path / "membership" / "known-nodes.bin", _config.forget_after, _config.time, _config.wall),
      _agreed(_identity.id, _config.state_path / "membership" / "agreed-nodes.bin"),
      _telemetry(_identity.id, _config.state_path / "telemetry" / "last-known.bin", _config.time, _config.wall, _config.telemetry_layout),
      _readiness(_config.planes, _config.wall),
      _activity(_config.time),
      _threads(std::chrono::seconds(1), std::chrono::seconds(60), _config.wall, _config.time, _config.log),
      _wait_guard(status::ScopedLogger(_config.log, "wait")),
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
  if (_config.memory_warning) status::ScopedLogger(_config.log, "node").warn(*_config.memory_warning);
  if (!_inbound.load_error().empty()) status::ScopedLogger(_config.log, "node").warn("inbound resolution ignored: " + _inbound.load_error());
  if (!_telemetry.load_error().empty()) status::ScopedLogger(_config.log, "node").warn("persisted telemetry ignored: " + _telemetry.load_error());
  // A node that has never held an agreed roster starts it from the nodes it
  // knows, so its first partition reading counts them all.
  if (!_agreed.found()) {
    std::vector<identity::NodeId> known;
    for (const auto& node : _members.all()) known.push_back(node.id);
    (void)_agreed.join(known);
    _agreed.flush();
  }
  // How this node proves itself: a certificate (version 2), the cluster key
  // (version 1), or both while a cluster moves.
  if (_config.certificate) {
    if (!_config.ca_certificate) throw std::invalid_argument("a node certificate needs the cluster's CA certificate (NodeConfig::ca_certificate)");
    if (!_custody) throw std::invalid_argument("a node certificate needs the node's key custody (NodeConfig::custody)");
    const auto& cert = *_config.certificate;
    if (cert.node_id() != _identity.id)
      throw std::invalid_argument("the certificate names node " + (cert.node_id() ? identity::to_string(*cert.node_id()) : std::string("none")) +
                                  ", and this node is " + identity::to_string(_identity.id));
    if (cert.public_key() != _custody->node_key().public_key()) throw std::invalid_argument("the certificate is not over this node's custody key");
    if (const auto why = identity::check_node_certificate(cert, *_config.ca_certificate, _config.wall->unix_ms()); !why.empty())
      throw std::invalid_argument("this node's certificate: " + why);
    _revocations.emplace(*_config.ca_certificate, _config.state_path / "identity" / "revocations");
    if (_revocations->revoked(cert)) throw std::invalid_argument("this node's certificate is revoked");
    net::CertificateCredential credential{*_config.ca_certificate, cert, _custody->node_key(), {}, _config.wall};
    credential.revoked = [this](const identity::Certificate& peer) { return _revocations->revoked(peer); };
    _credentials.certificate = std::move(credential);
  }
  if (_config.use_cluster_key || !_config.certificate) _credentials.key = _identity.key;
  net::Fanout::Hooks hooks;
  hooks.self = [this] { return _members.self(); };
  hooks.roster = [this] { return _members.active(); };
  hooks.incarnation = _identity.durability_epoch;
  hooks.advertise = [this](const std::string& key, std::optional<net::Bytes> value) {
    if (value)
      _members.set_advert(key, std::move(*value));
    else
      _members.clear_advert(key);
  };
  hooks.call = [this](const NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                      asio::any_io_executor executor, net::Client::Done done) {
    if (!_client || _stopping || _calls_cancelled) return asio::post(executor, [done] { done({std::nullopt, "the node is not running", {}}); });
    (void)place_call(peer, {peer.host, peer.port}, type, std::move(payload), frame, deadline, std::move(executor), std::move(done));
  };
  hooks.send = [this](const identity::NodeId& peer, const net::Message& message, sched::FrameType frame, bool best_effort) {
    return _client && !_stopping && _client->send(peer, net::Lane::control, message, frame, best_effort);
  };
  hooks.executor = _io.get_executor();
  hooks.replay_deadline = _config.connect_timeout * 2;
  _fanout = std::make_shared<net::Fanout>(std::move(hooks));
  // Tombstones loaded with the roster fence telemetry before anything runs.
  for (const auto& reset : _members.identity_resets()) _telemetry.apply_identity_reset(reset);
}

Node::~Node() { stop(); }

void Node::start() {
  // A cluster nobody could connect to is refused before anything listens.
  reach::refuse_impossible_cluster(_config.inbound, _config.bootstrap, _members.all(), _identity.id);
  if (_started.exchange(true)) return;
  // Before everything else, so a startup that never finishes still leaves its
  // windows behind.
  if (_config.observation_interval.count() > 0) {
    _observation_recorder = std::make_unique<status::ObservationRecorder>(
        _observations,
        std::make_shared<status::ObservationLog>(_config.state_path / "observation" / "observations.jsonl", _config.observation_max_bytes, _config.log),
        _config.version, _config.observation_interval, _config.observation_gauges, _config.wall, &_threads);
    _observations.event({_config.wall->unix_ms(), "start", {}, {{"version", _config.version}}});
    _observation_recorder->start();
  }
  _frozen = std::make_shared<const net::RouteTable>(_table);

  net::ServerOptions server_options;
  server_options.credentials = _credentials;
  server_options.local = [this] { return _members.self(); };
  server_options.max_frame = _config.max_frame;
  server_options.routes = _frozen;
  server_options.limits = _config.limits;
  server_options.memory = &_memory;
  server_options.traffic = _traffic;
  server_options.time = _config.time;
  server_options.log = _config.log;
  server_options.threads = &_threads;
  _server = std::make_unique<net::Server>(
      _io, _config.listen_host, _config.port, server_options,
      [this](const NodeRecord& peer, sched::FrameType frame, net::Message request, net::Answer& answer) {
        return handle(peer, frame, std::move(request), answer);
      },
      [this](const std::shared_ptr<net::Session>& session) {
        if (_client) _client->admit(session);
        note_certificate(session->established());
        observe_direct(session->peer());
      });

  net::ClientOptions client_options;
  client_options.credentials = _credentials;
  client_options.local = [this] { return _members.self(); };
  client_options.max_frame = _config.max_frame;
  client_options.routes = _frozen;
  client_options.memory = &_memory;
  client_options.traffic = _traffic;
  client_options.time = _config.time;
  client_options.connect_timeout = _config.connect_timeout;
  client_options.control_no_progress = _config.control_no_progress;
  client_options.data_no_progress = _config.data_no_progress;
  client_options.control_stall_notice = _config.control_stall_notice;
  client_options.data_stall_notice = _config.data_stall_notice;
  client_options.log = _config.log;
  client_options.dial_hook = _config.dial_hook;
  client_options.heartbeat = _config.heartbeat;
  client_options.dead_after = _config.dead_after;
  client_options.alive = [this](const NodeRecord& peer) { observe_direct(peer); };
  client_options.established = [this](const net::Established& session) {
    note_certificate(session);
    observe_direct(session.peer);
  };
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
  paths.credentials = _credentials;
  paths.threads = &_threads;
  paths.connect_timeout = _config.connect_timeout;
  paths.punch_window = _config.punch_window;
  paths.relay = _config.relay;
  paths.forced_class = _config.path_class;
  paths.upgrade_every = _config.path_upgrade_every;
  paths.max_data_circuits = _config.relay_data_circuits;
  // Chunks that have not come within the liveness window are not coming.
  paths.finish_wait = _config.relay_finish_wait.value_or(_config.dead_after);
  paths.drop_hook = _config.relay_drop_hook;
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
      _config.time, _config.log);

  _server->start();
  _bound_port = _server->bound_port();
  const auto self = _members.self();
  if (!self.host.empty() && self.port != _bound_port) _members.set_endpoint(self.host, _bound_port);
  _connectivity =
      std::make_unique<reach::PublicConnectivity>(_config.connectivity, _identity.id, net::Endpoint{_members.self().host, _bound_port}, _config.gateway,
                                                  _config.external_ip ? _config.external_ip : reach::ExternalIpLookup(reach::checkip_external_ip),
                                                  _config.self_probe ? _config.self_probe : reach::SelfProbe(reach::tcp_self_probe), _config.wall, _config.log);
  {
    contract::Lock lock(_refresh_mutex);
    _refreshes_open = true;
  }
  _readiness.close_declarations();
  _readiness.control_plane(true);
  _client->start();

  contract::Lock loops(_loops_mutex);
  _loops.emplace_back([this](std::stop_token stop) { _threads.loop("cluster-maintenance", stop, [this, stop] { membership_loop(stop); }); });
  _loops.emplace_back([this](std::stop_token stop) { _threads.loop("cluster-telemetry", stop, [this, stop] { telemetry_loop(stop); }); });
  _loops.emplace_back([this](std::stop_token stop) { _threads.loop("cluster-connectivity", stop, [this, stop] { connectivity_loop(stop); }); });
  // Law 1: an io thread learns of a peer and goes on; the disk is this thread's.
  _members.defer_observed_writes([this] {
    _roster_wanted.store(true);
    // As wake_loops: under the wait's mutex, so the wake is never lost.
    {
      std::lock_guard pass(_roster_mutex);
    }
    _roster_wake.notify_all();
  });
  _loops.emplace_back([this](std::stop_token stop) { _threads.loop("cluster-roster", stop, [this, stop] { roster_loop(stop); }); });
}

void Node::cancel_outbound_calls() {
  if (!_started || _calls_cancelled.exchange(true)) return;
  if (_client) _client->stop();
}

void Node::close_route(const identity::NodeId& peer, net::Lane lane, const std::string& why) {
  if (_client) _client->close_route(peer, lane, why);
}

void Node::request_stop() {
  {
    contract::Lock lock(_loops_mutex);
    if (!_loops.empty() && !_said_stopping.exchange(true)) status::ScopedLogger(_config.log, "node").debug("shutdown: node maintenance request_stop");
    for (auto& loop : _loops) loop.request_stop();
  }
  _wake.notify_all();
}

void Node::stop() {
  if (!_started || _stopping.exchange(true)) return;
  request_stop();
  // The transport goes first: a loop waiting on a call is released by it.
  if (_server) _server->stop();
  cancel_outbound_calls();
  if (_paths) _paths->stop();
  std::vector<std::jthread> loops;
  {
    contract::Lock lock(_loops_mutex);
    loops.swap(_loops);
  }
  for (auto& loop : loops)
    if (loop.joinable()) loop.join();
  // A clean stop leaves the last state on disk, and any later observation writes for itself.
  _members.defer_observed_writes({});
  try {
    _members.flush();
  } catch (const std::exception& error) {
    status::ScopedLogger(_config.log, "node").warn(std::string("known-node roster not written at stop: ") + error.what());
  }
  std::deque<ConnectivityRefresh> unserved;
  {
    contract::Lock lock(_refresh_mutex);
    _refreshes_open = false;
    unserved.swap(_refreshes);
  }
  for (auto& refresh : unserved) asio::post(refresh.executor, [done = std::move(refresh.done)] { done(std::nullopt); });
  _readiness.control_plane(false);
  // Last: the mapping this node made.
  _connectivity.reset();
  _observation_recorder.reset();
}

void Node::refresh_public_connectivity(bool probe, bool force_probe, asio::any_io_executor executor,
                                       std::function<void(std::optional<reach::PublicConnectivityStatus>)> done) {
  {
    contract::Lock lock(_refresh_mutex);
    if (!_refreshes_open) {
      asio::post(executor, [done = std::move(done)] { done(std::nullopt); });
      return;
    }
    _refreshes.push_back({probe, force_probe, std::move(executor), std::move(done)});
  }
  wake_loops();
}

void Node::wake_loops() {
  // A waiter tests its condition under _wait_mutex and then blocks; taking it
  // here means the change is seen either by that test or by the wake, never
  // lost between them.
  {
    std::lock_guard pass(_wait_mutex);
  }
  _wake.notify_all();
}

void Node::advertise(const reach::PublicConnectivityStatus& status) {
  const auto before = _members.self();
  if (!status.advertised.host.empty() && status.advertised.port && (status.advertised.host != before.host || status.advertised.port != before.port)) {
    _members.set_endpoint(status.advertised.host, status.advertised.port);
    status::ScopedLogger(_config.log, "node")
        .info("node advertised endpoint changed from=" + before.host + ":" + std::to_string(before.port) + " to=" + status.advertised.host + ":" +
              std::to_string(status.advertised.port) + " source=" + status.advertised_source);
  }
}

void Node::serve_refreshes() {
  while (true) {
    ConnectivityRefresh refresh;
    {
      contract::Lock lock(_refresh_mutex);
      if (_refreshes.empty()) return;
      refresh = std::move(_refreshes.front());
      _refreshes.pop_front();
    }
    auto status = _connectivity->refresh(refresh.probe, refresh.force_probe);
    advertise(status);
    asio::post(refresh.executor, [done = std::move(refresh.done), status = std::move(status)] { done(status); });
  }
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
  wake_loops();
}

net::Handled Node::handle(const NodeRecord& peer, sched::FrameType frame, net::Message request, net::Answer& answer) {
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
  if (type == m::revocations) {
    if (!_revocations) return net::Handled::failed(501, "no_certificates", "this node holds no certificates");
    try {
      (void)apply_revocations(identity::decode_revocations(request.payload));
    } catch (const net::DecodeError& error) {
      return net::Handled::failed(400, "invalid_request", error.what());
    }
    return net::Handled::processed({m::revocations_reply, identity::encode_revocations(_revocations->all()), {}});
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
  if (type == m::relay_credit) return _paths->relay_credit(peer, request);
  if (type == m::channel_publish) return _fanout->on_publish(peer, request);
  if (type == m::channel_replay) return _fanout->on_replay(peer, request);
  if (type == m::channel_bulk) return _fanout->on_bulk(peer, request);
  if (type == m::dial_back) return _dial_back->answer(peer.id, request.payload);
  // Held shared across the call: unbind waits for it.
  contract::ReadLock lock(_handlers_mutex);
  const auto handler = _handlers.find(type);
  if (handler == _handlers.end() || !handler->second) return net::Handled::unrecognised();
  return handler->second(peer, frame, std::move(request), answer);
}

const char* roster_change_name(RosterChange::Kind kind) noexcept {
  switch (kind) {
    case RosterChange::Kind::joined:
      return "joined";
    case RosterChange::Kind::left:
      return "left";
    case RosterChange::Kind::changed:
      return "changed";
  }
  return "unknown";
}

void Node::topology_changed(bool quiet) {
  std::vector<RosterChange> changes;
  bool membership = false;
  {
    contract::Lock lock(_watch_mutex);
    std::map<identity::NodeId, NodeRecord> now;
    for (auto& node : _members.active())
      if (node.id != _identity.id) now.emplace(node.id, std::move(node));
    for (const auto& [id, node] : now) {
      const auto before = _told.find(id);
      if (before == _told.end()) {
        changes.push_back({RosterChange::Kind::joined, node});
        membership = true;
      } else if (shape_changed(before->second, node)) {
        changes.push_back({RosterChange::Kind::changed, node});
        membership = membership || place_changed(before->second, node);
      }
    }
    for (const auto& [id, node] : _told)
      if (!now.contains(id)) {
        changes.push_back({RosterChange::Kind::left, node});
        membership = true;
      }
    std::vector<identity::NodeId> joined;
    for (const auto& change : changes)
      if (change.kind == RosterChange::Kind::joined) joined.push_back(change.node.id);
    _told = std::move(now);
    for (const auto& [_, watcher] : _watchers)
      for (const auto& change : changes) asio::post(watcher.executor, [handler = watcher.handler, change] { handler(change); });
    // A join adds to the agreed roster in memory; the roster loop writes it.
    if (_agreed.join(joined)) {
      _roster_wanted.store(true);
      _roster_wake.notify_all();
    }
    auto side = partition_locked();
    side.unix_ms = 0;
    auto told = _told_partition;
    if (told) told->unix_ms = 0;
    if (!told || *told != side) {
      const auto now_side = partition_locked();
      _told_partition = now_side;
      for (const auto& [_, watcher] : _partition_watchers) asio::post(watcher.executor, [handler = watcher.handler, now_side] { handler(now_side); });
    }
  }
  if (!quiet || !changes.empty()) _events.notify(status::NodeEvents::topology);
  if (membership) _events.notify(status::NodeEvents::membership);
}

membership::Partition Node::partition_locked() const {
  std::set<identity::NodeId> active{_identity.id};
  for (const auto& [id, _] : _told) active.insert(id);
  return membership::read_partition(_agreed.members(), active, _config.wall->unix_ms());
}

void Node::note_certificate(const net::Established& session) {
  if (!session.peer_certificate) return;
  contract::Lock lock(_certificates_mutex);
  _peer_certificates.insert_or_assign(session.peer.id, *session.peer_certificate);
}

std::optional<identity::Certificate> Node::peer_certificate(const identity::NodeId& node) const {
  std::optional<identity::Certificate> out;
  {
    contract::Lock lock(_certificates_mutex);
    if (const auto found = _peer_certificates.find(node); found != _peer_certificates.end()) out = found->second;
  }
  if (out && _revocations && _revocations->revoked(*out)) return std::nullopt;
  return out;
}

void Node::observe_direct(const NodeRecord& peer) {
  const auto before = _members.active();
  const auto previous = std::find_if(before.begin(), before.end(), [&](const NodeRecord& n) { return n.id == peer.id; });
  const bool changed = previous == before.end() || shape_changed(*previous, peer);
  _members.observe(peer, true);
  if (_client && peer.id != _identity.id) _client->note_peer(peer);
  if (changed) topology_changed();
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
  if (changed || ids_of(before_active) != ids_of(_members.active())) topology_changed();
}

std::shared_ptr<net::CallHandle> Node::place_call(std::optional<NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload,
                                                  sched::FrameType frame, std::chrono::milliseconds deadline, asio::any_io_executor executor,
                                                  net::Client::Done done, net::CallWatch watch) {
  if (!_started || _stopping || _calls_cancelled || !_client) {
    asio::post(executor, [done = std::move(done), why = std::string(_started ? "node is stopping; outbound calls are cancelled" : "node is not started")] {
      done(net::CallOutcome{std::nullopt, why, {}});
    });
    return std::make_shared<net::CallHandle>();
  }
  // Whichever comes first, the answer or the deadline, is the one told. At
  // the deadline the call is cancelled, so it holds no reply slot past it;
  // the deadline may pass before the client hands back the handle, so the
  // timer fires on the strand that hands it over.
  struct State {
    std::atomic<bool> finished{false};
    net::Client::Done done;
    // On the strand.
    std::shared_ptr<net::CallHandle> handle;
    bool expired = false;
  };
  auto state = std::make_shared<State>();
  state->done = std::move(done);
  auto strand = asio::make_strand(_io);
  auto timer = std::make_shared<asio::steady_timer>(strand);
  when_time_reaches(timer, *_config.time, _config.time->now() + deadline, [state, executor, deadline] {
    state->expired = true;
    // The deadline is claimed before the cancel, whose own "cancelled"
    // answer would otherwise race it to the caller.
    const bool answered = state->finished.exchange(true);
    if (state->handle) state->handle->cancel();
    if (answered) return;
    asio::post(executor, [state, deadline] {
      state->done(net::CallOutcome{std::nullopt, "deadline of " + std::to_string(deadline.count()) + " ms passed before an answer", {}});
    });
  });
  auto finish = [state, timer, strand](net::CallOutcome outcome) {
    asio::post(strand, [timer] { timer->cancel(); });
    if (state->finished.exchange(true)) return;
    state->done(std::move(outcome));
  };
  auto handle = peer ? _client->call(*peer, type, std::move(payload), frame, executor, std::move(finish), watch)
                     : _client->call(endpoint, type, std::move(payload), frame, executor, std::move(finish), watch);
  asio::post(strand, [state, handle] {
    state->handle = handle;
    if (state->expired) handle->cancel();
  });
  return handle;
}

net::CallOutcome Node::call_wait(std::optional<NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload,
                                 std::chrono::milliseconds deadline, sched::FrameType frame, net::CallWatch watch) {
  auto done = std::make_shared<std::promise<net::CallOutcome>>();
  auto future = done->get_future();
  place_call(
      std::move(peer), endpoint, type, std::move(payload), frame, deadline, _io.get_executor(),
      [done](net::CallOutcome outcome) { done->set_value(std::move(outcome)); }, watch);
  // The deadline answers first; the margin only covers the executor.
  if (future.wait_for(deadline + std::chrono::seconds(5)) != std::future_status::ready) return {std::nullopt, "no answer", {}};
  return future.get();
}

void Node::exchange(std::optional<NodeRecord> peer, const net::Endpoint& endpoint) {
  const auto outcome = call_wait(peer, endpoint, net::message::members, {}, _config.exchange_budget);
  if (!outcome.reply) throw std::runtime_error(outcome.failure);
  if (outcome.reply->type != net::message::members_reply) throw std::runtime_error("membership refused");
  merge(outcome.reply->payload);
  // Revocations ride the exchange: a node that missed one learns it within
  // a heartbeat.
  exchange_revocations(std::move(peer), endpoint, _config.exchange_budget);
}

void Node::sleep_until(std::stop_token stop, contract::TimePoint due) {
  std::unique_lock lock(_wait_mutex);
  const auto now = _config.time->now();
  if (due > now) _wake.wait_for(lock, stop, due - now, [] { return false; });
}

void Node::roster_loop(std::stop_token stop) {
  status::ThreadCpuReporter cpu("cluster-roster", std::chrono::seconds(5), status::ScopedLogger(_config.log, "node"));
  while (!stop.stop_requested()) {
    cpu.tick();
    _members.flush();
    _agreed.flush();
    std::unique_lock lock(_roster_mutex);
    // Observations made during a write combine into the next.
    _roster_wake.wait_for(lock, stop, std::chrono::seconds(1), [this] { return _roster_wanted.exchange(false); });
  }
}

void Node::membership_loop(std::stop_token stop) {
  status::ThreadCpuReporter cpu("cluster-maintenance", std::chrono::seconds(5), status::ScopedLogger(_config.log, "node"));
  while (!stop.stop_requested()) {
    cpu.tick();
    if (const auto forgotten = _members.forget_unseen()) {
      status::ScopedLogger(_config.log, "node").info("membership: forgot " + std::to_string(forgotten) + " node(s) not heard of for the absence horizon");
      topology_changed();
    } else {
      topology_changed(true);
    }
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
      } catch (const std::exception& error) {
        // A seed down is ordinary; the next heartbeat tries again.
        status::ScopedLogger(_config.log, "node").debug(std::string("bootstrap: ") + error.what());
      }
    }
    for (const auto& node : known) {
      if (stop.stop_requested()) return;
      if (node.id == _identity.id || !exchanged.emplace(node.host, node.port).second || unreachable(node)) continue;
      try {
        exchange(node, {node.host, node.port});
        ++active;
      } catch (const std::exception& error) {
        status::ScopedLogger(_config.log, "node").debug("peer " + node.host + ": " + error.what());
      }
    }
    _peers_active = active;
    sleep_until(stop, _config.time->now() + _config.heartbeat);
  }
}

void Node::telemetry_loop(std::stop_token stop) {
  status::ThreadCpuReporter cpu("cluster-telemetry", std::chrono::seconds(5), status::ScopedLogger(_config.log, "node"));
  const auto interval = std::max(_config.telemetry_interval, std::chrono::milliseconds(250));
  // Sampling follows demand; gossip at most once a second.
  const auto gossip_every = std::min(interval, std::chrono::milliseconds(1000));
  const auto ttl = std::max(_config.dead_after * 2, std::chrono::milliseconds(60000));
  contract::TimePoint last_gossip{};
  while (!stop.stop_requested()) {
    const auto demand = _telemetry_demand.load(std::memory_order_acquire);
    cpu.tick();
    // A reading on whichever io thread takes this: the busier the pool, the more
    // of them get a turn. Macha's per-connection threads have no equivalent here.
    asio::post(_io, [readings = _io_cpu] { readings->tick(); });
    try {
      status::LocalFigures figures;
      figures.self = _members.self();
      figures.version = _config.version;
      figures.phase = _readiness.phase();
      figures.peers_known = _peers_known;
      figures.peers_active = _peers_active;
      if (_client) {
        const auto rpc = _client->stats();
        figures.connections_created = rpc.connections_created;
        figures.connections_reused = rpc.reused;
        figures.connections_canonical = rpc.canonical_connections;
      }
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
    } catch (const std::exception& error) {
      // The next tick samples again.
      status::ScopedLogger(_config.log, "node").debug(std::string("telemetry refresh skipped: ") + error.what());
    }
    std::unique_lock lock(_wait_mutex);
    _wake.wait_for(lock, stop, interval, [&] { return _telemetry_demand.load(std::memory_order_acquire) != demand; });
  }
}

void Node::apply_inbound(bool capable) {
  const bool before = _members.self().inbound_capable();
  if (_members.set_inbound_capable(capable)) {
    topology_changed();
    signal_telemetry();
  }
  status::ScopedLogger(_config.log, "node")
      .info(std::string("node inbound resolution changed inbound_capable=") + (capable ? "true" : "false") +
            (_config.inbound_journal_fields ? _config.inbound_journal_fields(capable) : std::string{}) +
            " previous_inbound_capable=" + (before ? "true" : "false") + " source=" + _inbound.state().source);
}

std::chrono::milliseconds Node::classify() {
  const auto self = _members.self();
  bool changed = false;
  auto again = _config.heartbeat;
  if (_config.path_class) {
    // Forced at start, and left alone.
    return _config.reprobe_while_incapable;
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
    topology_changed();
    signal_telemetry();
  }
  return again;
}

void Node::connectivity_loop(std::stop_token stop) {
  advertise(_connectivity->refresh(_config.connectivity.check.enabled));
  const bool automatic = _config.inbound == reach::Inbound::automatic;
  auto next = _config.time->now();
  auto next_class = next;
  status::ThreadCpuReporter cpu("cluster-connectivity", std::chrono::seconds(5), status::ScopedLogger(_config.log, "node"));
  while (!stop.stop_requested()) {
    cpu.tick();
    // At least every heartbeat: the sweep runs on every tick.
    const auto tick = _config.time->now() + _config.heartbeat;
    {
      // Unlike sleep_until, a refresh someone asked for ends this wait.
      const auto due = std::min(automatic ? std::min(next, next_class) : next_class, tick);
      std::unique_lock lock(_wait_mutex);
      const auto now = _config.time->now();
      if (due > now)
        _wake.wait_for(lock, stop, due - now, [this] {
          contract::Lock refreshes(_refresh_mutex);
          return !_refreshes.empty();
        });
    }
    if (stop.stop_requested()) return;
    serve_refreshes();
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
    const auto outcome =
        call_wait(peer, {peer->host, peer->port}, net::message::dial_back, reach::encode_dial_back_request({self.host, self.port}), _config.exchange_budget);
    if (!outcome.reply || outcome.reply->type != net::message::dial_back_reply) {
      status::ScopedLogger(_config.log, "node")
          .debug("dial-back probe not answered peer=" + identity::to_string(peer->id).substr(0, 12) +
                 " error=" + (outcome.reply ? std::string("refused") : outcome.failure));
      continue;
    }
    try {
      const auto [reachable, error] = reach::decode_dial_back_reply(outcome.reply->payload);
      const auto verdict = _inbound.observe_probe(peer->id, reachable, error);
      status::ScopedLogger(_config.log, "node")
          .debug("dial-back probe peer=" + identity::to_string(peer->id).substr(0, 12) + " endpoint=" + self.host + ":" + std::to_string(self.port) +
                 " reachable=" + (reachable ? "true" : "false") + (error.empty() ? "" : " error=" + error) +
                 " consecutive_failures=" + std::to_string(_inbound.state().consecutive_failures));
      if (!verdict.persist_error.empty()) status::ScopedLogger(_config.log, "node").warn("inbound resolution not persisted: " + verdict.persist_error);
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
  if (changed) {
    status::ScopedLogger(_config.log, "node")
        .info("node identity association reset scope=" + reset.key() +
              " stale_node_id=" + (reset.stale_node_id == identity::NodeId{} ? std::string("<any>") : identity::to_string(reset.stale_node_id)) +
              " epoch=" + std::to_string(reset.epoch) + " reset_by=" + identity::to_string(reset.reset_by) +
              (reset.reason.empty() ? std::string{} : " reason=" + reset.reason));
    // The reset's stale id leaves the agreed roster too: it was another node.
    if (reset.stale_node_id != identity::NodeId{} && reset.stale_node_id != _identity.id) (void)_agreed.retire(reset.stale_node_id);
    topology_changed();
  }
  return changed;
}

std::vector<identity::Revocation> Node::apply_revocations(const std::vector<identity::Revocation>& revocations) {
  if (!_revocations) return {};
  const auto kept = _revocations->add(revocations);
  // At once: the revoked node's sessions close, and its next handshake is
  // refused.
  for (const auto& revocation : kept) {
    if (revocation.node == _identity.id) continue;
    if (_client) {
      _client->close_route(revocation.node, net::Lane::control, "revoked");
      _client->close_route(revocation.node, net::Lane::data, "revoked");
    }
  }
  if (!kept.empty()) topology_changed();
  return kept;
}

void Node::exchange_revocations(std::optional<NodeRecord> peer, const net::Endpoint& endpoint, std::chrono::milliseconds deadline) {
  if (!_revocations) return;
  const auto held = _revocations->all();
  if (held.empty()) return;
  const auto outcome = call_wait(std::move(peer), endpoint, net::message::revocations, identity::encode_revocations(held), deadline);
  if (!outcome.reply || outcome.reply->type != net::message::revocations_reply) return;
  try {
    (void)apply_revocations(identity::decode_revocations(outcome.reply->payload));
  } catch (const net::DecodeError&) {
  }
}

void Node::propagate_identity_reset(const membership::IdentityReset& reset, std::chrono::milliseconds deadline) {
  (void)apply_identity_reset(reset);
  const auto payload = membership::encode_identity_resets({reset});
  for (const auto& peer : _members.active()) {
    if (peer.id == _identity.id) continue;
    const auto outcome = call_wait(peer, {peer.host, peer.port}, net::message::identity_resets, payload, deadline);
    if (!outcome.reply || outcome.reply->type != net::message::identity_resets_reply) {
      status::ScopedLogger(_config.log, "node").debug("identity reset propagation to " + peer.host + ": " + (outcome.reply ? "refused" : outcome.failure));
      continue;
    }
    try {
      for (const auto& learned : membership::decode_identity_resets(outcome.reply->payload)) apply_identity_reset(learned);
    } catch (const net::DecodeError&) {
    }
  }
}

}  // namespace dannan
