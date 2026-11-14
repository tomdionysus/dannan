//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "dannan/contract/scope.h"
#include "dannan/contract/time_source.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/revocations.h"
#include "dannan/membership/agreed_roster.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/client.h"
#include "dannan/net/fanout.h"
#include "dannan/net/scheduler.h"
#include "dannan/net/server.h"
#include "dannan/placement/placement.h"
#include "dannan/reach/connectivity.h"
#include "dannan/reach/inbound.h"
#include "dannan/reach/paths.h"
#include "dannan/sched/retained_memory.h"
#include "dannan/sched/work.h"
#include "dannan/status/activity.h"
#include "dannan/status/events.h"
#include "dannan/status/log.h"
#include "dannan/status/observation.h"
#include "dannan/status/readiness.h"
#include "dannan/status/surface.h"
#include "dannan/status/telemetry.h"
#include "dannan/supervise/supervised.h"

// What a hosted system receives at start, and the only way it touches the
// network: the design's nouns (Nodes, Routes, Channels, Identity, Budget,
// Faults, Time, Status). Written as a specification a second implementation
// could satisfy: every operation states what it waits on and its thread
// safety, every completion is posted to the executor the caller passed, no
// call waits without a deadline, and nothing that happened on more than one
// node is reported as a bare boolean. Every noun is thread-safe.
namespace dannan {

// The contract's version. A hosted system states the version it was built
// against, and a mismatch is refused.
inline constexpr uint32_t context_version = 1;

struct RosterSnapshot {
  std::vector<membership::NodeRecord> all;
  std::vector<membership::NodeRecord> active;
};

// The cluster as this node knows it. Reads of local state wait on nothing;
// propagate_identity_reset waits on peers, each within the deadline.
// One change to the active roster: a node heard of anew, one not heard of
// for dead_after, or one whose place, capabilities, path class, adverts or
// incarnation changed. A new incarnation is a restart.
struct RosterChange {
  enum class Kind : uint8_t { joined, left, changed };
  Kind kind = Kind::joined;
  // As it is now; for a node that left, as it was last known.
  membership::NodeRecord node;
};
const char* roster_change_name(RosterChange::Kind) noexcept;

class Nodes {
 public:
  virtual ~Nodes() = default;
  // Runs a UPnP refresh, and a self-probe if `probe`, now rather than at the
  // next turn of the node's loop, and answers with the status on `executor`:
  // empty when the node is not running. Waits on nothing itself; the work is
  // the node's, bounded by the connectivity timeouts. Thread-safe.
  virtual void refresh_public_connectivity(bool probe, bool force_probe, boost::asio::any_io_executor executor,
                                           std::function<void(std::optional<reach::PublicConnectivityStatus>)> done) = 0;
  // The roster's changes, as they happen, posted to `executor` in order
  // when it is a strand: first a `joined` for every node active now, then
  // each change. An id for unwatch. Waits on nothing.
  virtual uint64_t watch(boost::asio::any_io_executor executor, std::function<void(RosterChange)> handler) = 0;
  virtual void unwatch(uint64_t id) = 0;
  virtual membership::NodeRecord self() const = 0;
  // Every known node, self first; and those heard of within dead_after.
  virtual std::vector<membership::NodeRecord> roster() const = 0;
  virtual std::vector<membership::NodeRecord> active() const = 0;
  // Both, from one reading.
  virtual RosterSnapshot snapshot() const = 0;
  // When a node not heard of within dead_after (directly or by gossip) was
  // last heard of, on the node's steady clock (Time::source()); none for
  // this node, an active one, or one not known. It survives a restart: the
  // roster keeps the wall time each node was last heard of, and a node
  // forgotten at the horizon is no longer known. Waits on nothing.
  virtual std::optional<contract::TimePoint> absent_since(const identity::NodeId& node) const = 0;
  // The certificate `node` proved in its latest certificate handshake with
  // this node (wire version 2), checked against the CA then; none for a
  // node never met that way, one met only by cluster key, or one revoked
  // since. Its public key verifies what the node signed. Waits on nothing.
  virtual std::optional<identity::Certificate> certificate(const identity::NodeId& node) const = 0;
  // Authenticated directly within dead_after; gossip never counts.
  virtual bool directly_reachable(const identity::NodeId& node) const = 0;
  // Every known node directly reachable, pairs that cannot meet excepted.
  virtual bool all_known_reachable() const = 0;
  virtual bool inbound_capable(const identity::NodeId& node) const = 0;
  // Why this node's route to `node` on `lane` was opened: ordinary (direct
  // or reverse), punched or relayed; nothing without a route.
  virtual std::optional<net::Purpose> path(const identity::NodeId& node, net::Lane lane) const = 0;
  // The smoothed CONTROL round trip, from health pings only.
  virtual std::optional<std::chrono::milliseconds> latency(const identity::NodeId& node) const = 0;
  virtual std::map<identity::NodeId, std::chrono::milliseconds> latencies() const = 0;
  // Identity association resets: held, applied here (roster, telemetry,
  // routes), and propagated to every active peer, each asked within
  // `deadline`, their answers applied too.
  virtual std::vector<membership::IdentityReset> identity_resets() const = 0;
  virtual bool apply_identity_reset(const membership::IdentityReset& reset) = 0;
  virtual void propagate_identity_reset(const membership::IdentityReset& reset, std::chrono::milliseconds deadline) = 0;
  // Ordered candidates for `key` over the active roster under the hosted
  // system's scheme: the first `copies` preferred, the rest fallbacks. With
  // `only_advertising`, only nodes carrying that advert are candidates.
  // Throws std::invalid_argument for a scheme without its salts.
  virtual std::vector<membership::NodeRecord> placement(std::span<const uint8_t> key, size_t copies, const placement::Scheme& scheme,
                                                        const std::string& only_advertising = {}) const = 0;
  // This node's adverts: the hosted system's figures, carried in its record
  // and gossiped with it; Dannan never reads them unless a caller names
  // one. A change makes the record newer. Throws std::invalid_argument for
  // an empty key or one over 256 bytes, a value over 1 KiB, or a 65th.
  virtual void advertise(const std::string& key, net::Bytes value) = 0;
  virtual void withdraw(const std::string& key) = 0;

  // Which side of a split this node is on (P2; Tom, 2026-11-13), read against
  // the roster as last agreed: a majority only with more than half of it
  // reachable, this node counted, so an even split is a minority. One rule for
  // every hosted system. Nodes joined during a split can give both sides a
  // majority (membership::Partition says how): join nodes to a whole cluster.
  // Waits on nothing.
  virtual membership::Partition partition() const = 0;
  // Its changes, posted to `executor` in order when it is a strand: first the
  // reading now, then each change of side, count or who is unreachable. An id
  // for unwatch_partition. Waits on nothing.
  virtual uint64_t watch_partition(boost::asio::any_io_executor executor, std::function<void(membership::Partition)> handler) = 0;
  virtual void unwatch_partition(uint64_t id) = 0;
  // The roster as last agreed, this node included. A node joins it when it is
  // seen to join; nothing but retire (or an identity reset of its id) takes it
  // out, never absence.
  virtual std::set<identity::NodeId> members() const = 0;
  // An operator's word that `node` has left the cluster for good: out of the
  // agreed roster, durably, so the partition reading no longer counts it. A
  // node retired while still active is a member again when it is next seen to
  // join, a restart included. False if it was not a member. Throws
  // std::invalid_argument for this node. Waits on the state device.
  virtual bool retire(const identity::NodeId& node) = 0;
};

// Message types and calls. A hosted system registers its types (their wire
// rules) before the node starts; Dannan's core types are not its to take.
// Handlers are bound and unbound at any time, as a subsystem starts and
// stops; a type with no handler is answered 501.
class Routes {
 public:
  using Done = net::Client::Done;
  virtual ~Routes() = default;
  // Registers a type's wire rules and, if given, its handler. Throws
  // std::logic_error after start or for a core type, std::invalid_argument
  // for a number taken or retired. Register a type on every node that calls
  // or answers it, not only where it is handled: a caller without its rules
  // sends it on the control lane, and a peer whose rules put it on the data
  // lane refuses the frame by ending the session, failing every call on it
  // (Tom, 2026-11-14: the rule is documented, the wire unchanged).
  virtual void add(net::MessageType type, net::Registration registration, net::RequestHandler handler = {}) = 0;
  // Marks a number as no longer used; add() refuses it from then on. Before start.
  virtual void retire(net::MessageType type) = 0;
  // Binds the handler for a registered type; replaces one bound.
  virtual void bind(net::MessageType type, net::RequestHandler handler) = 0;
  // Binds one that may answer later (P8): it moves the request's Answer out
  // and returns Handled::deferred(), and gives the Answer from any thread,
  // holding no worker meanwhile. Bounded per class by the node's
  // ServerLimits::max_deferred; past it the class's requests are refused to
  // try later. Replaces one bound.
  virtual void bind_deferred(net::MessageType type, net::DeferringHandler handler) = 0;
  // Unbinds it. Waits for the handler's calls in flight to return.
  virtual void unbind(net::MessageType type) = 0;
  // Calls `peer`, dialling if need be; `done` is posted to `executor` once:
  // the reply, or why none came, at the latest when `deadline` passes. A
  // call whose deadline passes is cancelled: its reply slot is freed and the
  // peer told, and a reply that comes later is dropped. The handle controls
  // the call meanwhile. Waits on nothing.
  // `watch` names this call's own no-progress deadline and stall notice; a call
  // with none is unwatched unless its type or the node's settings say otherwise.
  virtual std::shared_ptr<net::CallHandle> call(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done,
                                                net::CallWatch watch = {}) = 0;
  virtual std::shared_ptr<net::CallHandle> call(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done,
                                                net::CallWatch watch = {}) = 0;
  // The same call, waited for: the outcome, at the latest when `deadline`
  // passes. Waits on the peer. Never from one of the io context's threads.
  virtual net::CallOutcome call_and_wait(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                         std::chrono::milliseconds deadline, net::CallWatch watch = {}) = 0;
  virtual net::CallOutcome call_and_wait(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                         std::chrono::milliseconds deadline, net::CallWatch watch = {}) = 0;
};

// Channels. Named channels with a delivery contract chosen when opened
// (notify, stream, bulk: net/fanout.h), subscribed to on this node by
// topic prefix, the subscriptions gossiped, so a publish goes only where
// someone listens. And the broadcasts: a notification to every peer over
// CONTROL, each returning how many peers' queues took it; none dials.
class Channels {
 public:
  virtual ~Channels() = default;
  // Throws std::invalid_argument for an empty name, one over 64 bytes, or
  // one already open. `replay_bytes` bounds a stream's replay buffer.
  virtual void open(std::string name, net::Delivery delivery, size_t replay_bytes = 1 << 20) = 0;
  // Deliveries to `handler` are posted to `executor`, in order per origin
  // when it is a strand. Throws std::invalid_argument for a channel not open.
  virtual uint64_t subscribe(const std::string& channel, std::string prefix, boost::asio::any_io_executor executor, net::ChannelHandler handler) = 0;
  virtual void unsubscribe(uint64_t id) = 0;
  // notify and stream: sent now, never waiting; where it went is the
  // evidence. A destination with no route yet is reported, and a route is
  // made for the next publish; a stream message is held for its replay.
  virtual net::Published publish(const std::string& channel, std::string topic, net::Bytes payload) = 0;
  // bulk: `done` posted to `executor` once every destination acknowledged
  // or `deadline` passed.
  virtual void publish_bulk(const std::string& channel, std::string topic, net::Bytes payload, std::chrono::milliseconds deadline,
                            boost::asio::any_io_executor executor, std::function<void(net::Published)> done) = 0;
  // In the control class, queued behind whatever is there; waits for a
  // queue that is full, never for a peer.
  virtual size_t broadcast(const net::Message& message) = 0;
  // Only where it adds no delay; sends nothing while the route table is
  // busy, so a hot path never waits.
  virtual size_t broadcast_best_effort(const net::Message& message, sched::FrameType frame) = 0;
  // As best effort, but waits for the route table.
  virtual size_t notify(const net::Message& message, sched::FrameType frame) = 0;
};

// Every node's latest telemetry, this node's included. Reads wait on
// nothing; persist waits on the file.
class Telemetry {
 public:
  virtual ~Telemetry() = default;
  virtual std::optional<status::Telemetry> local() const = 0;
  virtual std::vector<status::Telemetry> all() const = 0;
  virtual std::vector<status::TelemetryView> views(std::chrono::milliseconds fresh_for) const = 0;
  virtual std::vector<status::Telemetry> persisted() const = 0;
  virtual void persist() = 0;
  // Asks for the local record to be refreshed and gossiped now.
  virtual void refresh() = 0;
};

// What a Status page reads of the node, in one snapshot.
struct Diagnostics {
  uint64_t started_unix_ms = 0;
  std::optional<reach::PublicConnectivityStatus> connectivity;
  reach::InboundState inbound;
  net::ClientStats client;
  net::WorkStats work;
  status::TrafficTotals traffic;
  std::vector<supervise::SupervisedThreadStatus> threads;
  // Level 0: punches and relays tried and made, and the circuits through
  // this node with their bytes.
  reach::PathStats paths;
};

// Who this node is.
class Identity {
 public:
  virtual ~Identity() = default;
  virtual identity::NodeId id() const = 0;
  // Fresh every process: peers tell a restart from a reconnect.
  virtual identity::NodeId durability_epoch() const = 0;
  // A key for the hosted system's own use, the same on every node of the
  // cluster: derived from the cluster key under its label, in secure memory.
  virtual identity::Secret derive_key(std::string_view label) const = 0;

  // The node's own key, from its custody (NodeConfig::custody), when it has
  // one: its public key (SubjectPublicKeyInfo; empty without custody), a
  // signature by it, and keys for node-local use derived from custody's
  // seal root under a label (Dagda's node-local spaces). The last two throw
  // std::logic_error without custody.
  virtual bool has_custody() const = 0;
  virtual identity::Bytes node_public_key() const = 0;
  virtual identity::Bytes sign(std::span<const uint8_t> message) const = 0;
  virtual identity::Secret derive_local(std::string_view label) const = 0;

  // Certificates (wire version 2): this node's, when it has one; and a
  // revocation the cluster's CA signed, applied here at once (the revoked
  // node's sessions closed) and passed to every active peer, each asked
  // within the node's connect timeout. Throws std::invalid_argument for a
  // revocation the CA did not sign, std::logic_error on a node without
  // certificates.
  virtual std::optional<identity::Certificate> certificate() const = 0;
  // The cluster's CA certificate, on a node with certificates.
  virtual std::optional<identity::Certificate> ca_certificate() const = 0;
  virtual void revoke(const identity::Revocation& revocation) = 0;
  virtual std::vector<identity::Revocation> revocations() const = 0;

  // Scope (contract/scope.h): the source the directory installs (Lugh), and
  // the check every hosted system makes through it before it stores a write
  // (Dagda's write-floor replicas among them). Without a source, this node
  // and any node of the cluster may write, and nobody else. Never waits.
  virtual void install_scope_source(std::shared_ptr<const contract::ScopeSource> source) = 0;
  virtual contract::ScopeDecision verify(const contract::Principal& principal, std::string_view prefix) const = 0;
};

// Memory, by work class.
class Budget {
 public:
  virtual ~Budget() = default;
  // A unit of work the class must always be able to hold; throws
  // std::invalid_argument if the node's budget cannot hold one.
  virtual void declare(sched::WorkClass work, uint64_t unit_bytes) = 0;
  // Bytes held until the lease goes; nothing when the class has no room
  // now. Waits on nothing.
  virtual std::optional<sched::RetainedMemoryLedger::Lease> reserve(sched::WorkClass work, uint64_t bytes) = 0;
};

// Faults are values: raised, counted in Status, and handed to the sink the
// supervisor installs, which restarts or disables.
class Faults {
 public:
  using Sink = std::function<void(std::string reason)>;
  virtual ~Faults() = default;
  virtual void sink(Sink sink) = 0;
  virtual void raise(std::string reason) = 0;
};

// The node's injected clocks, its executor, and a timer.
class Time {
 public:
  virtual ~Time() = default;
  virtual contract::TimePoint now() const = 0;
  virtual uint64_t unix_ms() const = 0;
  // The time source behind now(), for a component that takes one: a test's
  // stepped source reaches the hosted system through here. Lives as long as
  // the node.
  virtual const contract::TimeSource& source() const = 0;
  // The node's io context's executor, for a hosted system's timers, strands
  // and completions. Its handlers must not block.
  virtual boost::asio::any_io_executor executor() const = 0;
  // Posts `fn` to the node's executor after `delay`. Waits on nothing.
  virtual void after(std::chrono::milliseconds delay, std::function<void()> fn) = 0;
};

class Context {
 public:
  virtual ~Context() = default;
  virtual uint32_t version() const noexcept = 0;
  virtual Nodes& nodes() = 0;
  virtual const Nodes& nodes() const = 0;
  virtual Routes& routes() = 0;
  virtual Channels& channels() = 0;
  virtual Identity& identity() = 0;
  virtual const Identity& identity() const = 0;
  virtual Budget& budget() = 0;
  virtual Faults& faults() = 0;
  virtual Time& time() = 0;
  virtual Telemetry& telemetry() = 0;
  // What the hosted system publishes, and its counters, for Status.
  virtual status::Surface& status() = 0;
  // A logger on the node's one sink under `scope`, the hosted system's own
  // short name ("dagda", "lugh.oauth"). There is no process-wide logger.
  virtual status::ScopedLogger logger(std::string scope) const = 0;
  // Monotonic counts per kind; topology is Dannan's.
  virtual status::NodeEvents& events() = 0;
  // The planes a hosted system declares and reports ready.
  virtual status::Readiness& readiness() = 0;
  // When work of each class was last present; the node notes each request.
  virtual status::ActivityClocks& activity() = 0;
  // Measurement probes, written to the node's observation log.
  virtual status::Observations& observations() = 0;
  // Checks a work context against the waits an operation declares: the node's
  // one guard, so its violations are counted together.
  virtual sched::WaitGuard& wait_guard() = 0;
  // The node's thread bodies, and the hosted system's, under one record.
  virtual supervise::SupervisedThreads& threads() = 0;
  virtual Diagnostics diagnostics() const = 0;
};

}  // namespace dannan
