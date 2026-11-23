//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// What a hosted system receives at start: the Context and its nouns.

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

#include "dannan/agree/agreement.h"
#include "dannan/contract/hlc.h"
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
#include "dannan/sched/buffer_pool.h"
#include "dannan/sched/class_gate.h"
#include "dannan/sched/retained_memory.h"
#include "dannan/sched/work.h"
#include "dannan/sched/worker_pool.h"
#include "dannan/status/activity.h"
#include "dannan/status/events.h"
#include "dannan/status/log.h"
#include "dannan/status/observation.h"
#include "dannan/status/readiness.h"
#include "dannan/status/surface.h"
#include "dannan/status/telemetry.h"
#include "dannan/supervise/supervised.h"

namespace dannan {

/// The contract's version. A hosted system states the version it was built against, and a mismatch
/// is refused.
inline constexpr uint32_t context_version = 1;

/// Every known node and the active ones, from one reading.
struct RosterSnapshot {
  /// Every known node, self first.
  std::vector<membership::NodeRecord> all;
  /// Those heard of within dead_after.
  std::vector<membership::NodeRecord> active;
};

/// One change to the active roster: a node heard of anew, one not heard of for dead_after, or one
/// whose place, capabilities, path class, adverts or incarnation changed. A new incarnation is a
/// restart.
struct RosterChange {
  /// What happened.
  enum class Kind : uint8_t {
    /// Heard of anew, or active at the watch's start.
    joined,
    /// Not heard of for dead_after.
    left,
    /// Its record changed.
    changed,
  };
  /// What happened.
  Kind kind = Kind::joined;
  /// The node as it is now; for a node that left, as it was last known.
  membership::NodeRecord node;
};
/// "joined", "left" or "changed".
const char* roster_change_name(RosterChange::Kind) noexcept;

/// One peer's advert under a watched key (Nodes::watch_advert()).
struct AdvertChange {
  /// The peer.
  identity::NodeId node;
  /// Its value now; nothing once it is withdrawn or the peer has left the active roster.
  std::optional<net::Bytes> value;
};

/// The cluster as this node knows it: the roster, reachability and latency, adverts, placement and
/// the partition reading. Reads of local state wait on nothing; every member is thread-safe.
class Nodes {
 public:
  virtual ~Nodes() = default;
  /// Runs a UPnP refresh, and a self-probe if `probe`, now rather than at the next turn of the node's
  /// loop. Waits on nothing itself; the work is the node's, bounded by the connectivity timeouts.
  /// @param force_probe probe even if one ran recently.
  /// @param done the status, posted to `executor`; empty when the node is not running.
  virtual void refresh_public_connectivity(bool probe, bool force_probe, boost::asio::any_io_executor executor,
                                           std::function<void(std::optional<reach::PublicConnectivityStatus>)> done) = 0;
  /// The roster's changes as they happen, posted to `executor`, in order when it is a strand: first
  /// a `joined` for every node active now, then each change. Waits on nothing.
  /// @return an id for unwatch().
  virtual uint64_t watch(boost::asio::any_io_executor executor, std::function<void(RosterChange)> handler) = 0;
  /// Stops a roster watch.
  virtual void unwatch(uint64_t id) = 0;
  /// One advert's changes on the peers (P6). Adverts are slow facts, so this is rate-limited, where
  /// the roster watch tells every change of the record.
  ///
  /// Posted to `executor`, in order when it is a strand: first the value of every active peer
  /// carrying `key`, then each change, a peer's values at most once each `at_most_every`. Values
  /// inside that coalesce: the latest is told after it, by the next heartbeat. A withdrawal or a
  /// leave (no value) is never held back or coalesced away, so a peer that leaves and rejoins
  /// inside the period is told as both. Waits on nothing.
  /// @return an id for unwatch_advert().
  virtual uint64_t watch_advert(const std::string& key, std::chrono::milliseconds at_most_every, boost::asio::any_io_executor executor,
                                std::function<void(AdvertChange)> handler) = 0;
  /// Stops an advert watch.
  virtual void unwatch_advert(uint64_t id) = 0;
  /// This node's record.
  virtual membership::NodeRecord self() const = 0;
  /// Every known node, self first.
  virtual std::vector<membership::NodeRecord> roster() const = 0;
  /// The nodes heard of within dead_after, directly or by gossip.
  virtual std::vector<membership::NodeRecord> active() const = 0;
  /// Both, from one reading.
  virtual RosterSnapshot snapshot() const = 0;
  /// When a node not heard of within dead_after was last heard of, on the node's steady clock
  /// (Time::source()). It survives a restart: the roster keeps the wall time each node was last
  /// heard of, and a node forgotten at the horizon is no longer known. Waits on nothing.
  /// @return nothing for this node, an active one, or one not known.
  virtual std::optional<contract::TimePoint> absent_since(const identity::NodeId& node) const = 0;
  /// The certificate `node` proved in its latest certificate handshake with this node (wire version
  /// 2), checked against the CA then; its public key verifies what the node signed.
  /// @return nothing for a node never met that way, one met only by cluster key, or one revoked
  ///   since.
  virtual std::optional<identity::Certificate> certificate(const identity::NodeId& node) const = 0;
  /// Whether `node` was authenticated directly within dead_after; gossip never counts.
  virtual bool directly_reachable(const identity::NodeId& node) const = 0;
  /// Whether every known node is directly reachable, pairs that cannot meet excepted.
  virtual bool all_known_reachable() const = 0;
  /// Whether `node` accepts inbound connections.
  virtual bool inbound_capable(const identity::NodeId& node) const = 0;
  /// Why this node's route to `node` on `lane` was opened: ordinary (direct or reverse), punched or
  /// relayed; nothing without a route.
  virtual std::optional<net::Purpose> path(const identity::NodeId& node, net::Lane lane) const = 0;
  /// The smoothed CONTROL round trip to `node`, from health pings only; nothing before one.
  virtual std::optional<std::chrono::milliseconds> latency(const identity::NodeId& node) const = 0;
  /// Every measured round trip.
  virtual std::map<identity::NodeId, std::chrono::milliseconds> latencies() const = 0;
  /// This node's clock offset to `node`, from the clock probes made each heartbeat (positive: the
  /// peer's clock is ahead); nothing before a probe has come back.
  virtual std::optional<std::chrono::milliseconds> clock_offset(const identity::NodeId& node) const = 0;
  /// Whether this node's clock is within bounds: false while its offset is past
  /// NodeConfig::clock_offset_bound against more than half of the agreed roster, itself counted at
  /// zero, and then it issues no hybrid-clock stamp (hlc().now() throws
  /// contract::ClockOutOfBounds).
  virtual bool clock_in_bounds() const = 0;
  /// The identity association resets held.
  virtual std::vector<membership::IdentityReset> identity_resets() const = 0;
  /// Applies a reset here (roster, telemetry, routes).
  /// @return whether it was new.
  virtual bool apply_identity_reset(const membership::IdentityReset& reset) = 0;
  /// Applies a reset here and sends it to every active peer, each asked within `deadline`, their
  /// answers applied too. Waits on the peers, each within the deadline.
  virtual void propagate_identity_reset(const membership::IdentityReset& reset, std::chrono::milliseconds deadline) = 0;
  /// Ordered candidates for `key` over the active roster under the hosted system's scheme
  /// (placement::by_capacity()): the first `copies` preferred, the rest fallbacks, nodes advertising
  /// draining or not_ready demoted unless the scheme ignores health.
  /// @param only_advertising when given, only nodes carrying that advert are candidates.
  /// @throws std::invalid_argument for a scheme without its salts.
  virtual std::vector<membership::NodeRecord> placement(std::span<const uint8_t> key, size_t copies, const placement::Scheme& scheme,
                                                        const std::string& only_advertising = {}) const = 0;
  /// Sets one of this node's adverts: the hosted system's figures, carried in its record and gossiped
  /// with it. Dannan never reads them unless a caller names one. A change makes the record newer.
  /// @throws std::invalid_argument for an empty key or one over 256 bytes, a value over 1 KiB, or a
  ///   65th advert.
  virtual void advertise(const std::string& key, net::Bytes value) = 0;
  /// Withdraws one of this node's adverts.
  virtual void withdraw(const std::string& key) = 0;
  /// Sets `key` to `value`, or withdraws it (none), then sends this node's record straight to every
  /// active peer rather than at the heartbeat's pace; each merges it before it answers (for Lir). A
  /// peer that connects later has it from the handshake. Waits on nothing.
  /// @param done posted to `executor` once every peer answered or `deadline` passed: which peers
  ///   hold the record as pushed or newer, which did not and why.
  /// @throws std::invalid_argument as advertise() does.
  virtual void advertise(const std::string& key, std::optional<net::Bytes> value, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                         std::function<void(membership::Pushed)> done) = 0;

  /// Which side of a split this node is on (P2), read against the roster as last agreed: a majority
  /// only with more than half of it reachable, this node counted, so an even split is a minority.
  /// One rule for every hosted system. Nodes joined during a split can give both sides a majority
  /// (membership::Partition says how): join nodes to a whole cluster. Waits on nothing.
  virtual membership::Partition partition() const = 0;
  /// The partition reading's changes, posted to `executor`, in order when it is a strand: first the
  /// reading now, then each change of side, count or who is unreachable. Waits on nothing.
  /// @return an id for unwatch_partition().
  virtual uint64_t watch_partition(boost::asio::any_io_executor executor, std::function<void(membership::Partition)> handler) = 0;
  /// Stops a partition watch.
  virtual void unwatch_partition(uint64_t id) = 0;
  /// The roster as last agreed, this node included. A node joins it when it is seen to join; nothing
  /// but retire() (or an identity reset of its id) takes it out, never absence.
  virtual std::set<identity::NodeId> members() const = 0;
  /// An operator's word that `node` has left the cluster for good: out of the agreed roster,
  /// durably, so the partition reading no longer counts it. A node retired while still active is a
  /// member again when it is next seen to join, a restart included. Waits on the state device.
  /// @return false if it was not a member.
  /// @throws std::invalid_argument for this node.
  virtual bool retire(const identity::NodeId& node) = 0;
};

/// Message types and calls.
///
/// A hosted system registers its types (their wire rules) before the node starts, on every node
/// that calls or answers them; Dannan's core types are not its to take. Handlers are bound and
/// unbound at any time, as a subsystem starts and stops; a type with no handler is answered 501.
/// Every member is thread-safe.
class Routes {
 public:
  /// Where a call's outcome goes.
  using Done = net::Client::Done;
  virtual ~Routes() = default;
  /// Registers a type's wire rules and, if given, its handler. Register a type on every node that
  /// calls or answers it, not only where it is handled: a caller without its rules sends it on the
  /// control lane, and a peer whose rules put it on the data lane refuses the frame by ending the
  /// session, failing every call on it.
  /// @throws std::logic_error after the node starts, or for a core type.
  /// @throws std::invalid_argument for a number taken or retired.
  virtual void add(net::MessageType type, net::Registration registration, net::RequestHandler handler = {}) = 0;
  /// Marks a number as no longer used; add() refuses it from then on. Before start.
  virtual void retire(net::MessageType type) = 0;
  /// Binds the handler for a registered type; replaces one bound.
  virtual void bind(net::MessageType type, net::RequestHandler handler) = 0;
  /// Binds a handler that may answer later (P8): it moves the request's net::Answer out, returns
  /// net::Handled::deferred(), and gives the Answer from any thread, holding no worker meanwhile.
  /// Bounded per class by ServerLimits::max_deferred, and per peer by its share; past them the
  /// requests are refused to try later. Replaces one bound.
  virtual void bind_deferred(net::MessageType type, net::DeferringHandler handler) = 0;
  /// Unbinds a type's handler. Waits for the handler's calls in flight to return.
  virtual void unbind(net::MessageType type) = 0;
  /// Calls `peer`, dialling if need be. Waits on nothing.
  ///
  /// A call whose deadline passes is cancelled: its reply slot is freed and the peer told, and a
  /// reply that comes later is dropped.
  /// @param frame the call's work class, which picks its lane's queue and the peer's workers.
  /// @param deadline at it at the latest, `done` is told why no reply came.
  /// @param done posted to `executor` once: the reply, or why none came.
  /// @param watch this call's own no-progress deadline and stall notice; a call with none is
  ///   unwatched unless its type or the node's settings say otherwise.
  /// @return the handle that controls the call meanwhile (promote, cancel).
  virtual std::shared_ptr<net::CallHandle> call(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done,
                                                net::CallWatch watch = {}) = 0;
  /// The same, to an address rather than a known node.
  virtual std::shared_ptr<net::CallHandle> call(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, Done done,
                                                net::CallWatch watch = {}) = 0;
  /// The same call, waited for: the outcome, at the latest when `deadline` passes. Waits on the
  /// peer; never from one of the io context's threads.
  virtual net::CallOutcome call_and_wait(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                         std::chrono::milliseconds deadline, net::CallWatch watch = {}) = 0;
  /// The same, to an address.
  virtual net::CallOutcome call_and_wait(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame,
                                         std::chrono::milliseconds deadline, net::CallWatch watch = {}) = 0;
};

/// Channels and broadcasts.
///
/// Named channels with a delivery contract chosen when opened (net::Delivery: notify, stream, bulk,
/// ordered), subscribed to on this node by topic prefix, the subscriptions gossiped, so a publish
/// goes only where someone listens (net::Fanout has the detail). And the broadcasts: a notification
/// to every peer over CONTROL, each returning how many peers' queues took it; none dials. Every
/// member is thread-safe.
class Channels {
 public:
  virtual ~Channels() = default;
  /// Opens a channel on this node; every node that publishes or subscribes opens it alike.
  /// @param replay_bytes bounds a stream's replay buffer; 0 for an ordered channel.
  /// @throws std::invalid_argument for an empty name, one over 64 bytes, or one already open.
  virtual void open(std::string name, net::Delivery delivery, size_t replay_bytes = 1 << 20) = 0;
  /// Subscribes to every topic under `prefix` (empty: all). Deliveries are posted to `executor`, in
  /// order per origin when it is a strand.
  /// @return an id for unsubscribe().
  /// @throws std::invalid_argument for a channel not open.
  virtual uint64_t subscribe(const std::string& channel, std::string prefix, boost::asio::any_io_executor executor, net::ChannelHandler handler) = 0;
  /// The same, and the channel's interest, this prefix included, is pushed to every active peer at
  /// once (Nodes::advertise() with a deadline): `done` is posted to `done_executor` with which peers
  /// hold it, so a publish made after it reaches this subscriber. Hold an acknowledgement on it (Lir's
  /// SUBACK).
  virtual uint64_t subscribe(const std::string& channel, std::string prefix, boost::asio::any_io_executor executor, net::ChannelHandler handler,
                             std::chrono::milliseconds deadline, boost::asio::any_io_executor done_executor, std::function<void(membership::Pushed)> done) = 0;
  /// Ends a subscription; a delivery already posted may still arrive.
  virtual void unsubscribe(uint64_t id) = 0;
  /// Publishes on a notify, stream or ordered channel: sent now, never waiting. A destination with no
  /// route yet is reported, and a route is made for the next publish; a stream message is held for
  /// its replay.
  /// @return where it went: the evidence.
  virtual net::Published publish(const std::string& channel, std::string topic, net::Bytes payload) = 0;
  /// Publishes on a bulk channel: `done` is posted to `executor` once every destination has
  /// acknowledged or `deadline` has passed.
  virtual void publish_bulk(const std::string& channel, std::string topic, net::Bytes payload, std::chrono::milliseconds deadline,
                            boost::asio::any_io_executor executor, std::function<void(net::Published)> done) = 0;
  /// Sends `message` to every peer in the control class, queued behind whatever is there; waits for
  /// a queue that is full, never for a peer.
  /// @return how many peers' queues took it.
  virtual size_t broadcast(const net::Message& message) = 0;
  /// The same only where it adds no delay: sends nothing while the route table is busy, so a hot
  /// path never waits.
  virtual size_t broadcast_best_effort(const net::Message& message, sched::FrameType frame) = 0;
  /// As broadcast_best_effort(), but waits for the route table.
  virtual size_t notify(const net::Message& message, sched::FrameType frame) = 0;
};

/// Every node's latest telemetry, this node's included. Reads wait on nothing; persist() waits on
/// the file. Thread-safe.
class Telemetry {
 public:
  virtual ~Telemetry() = default;
  /// This node's record; nothing before the first.
  virtual std::optional<status::Telemetry> local() const = 0;
  /// Every node's latest record.
  virtual std::vector<status::Telemetry> all() const = 0;
  /// Every record, each read as fresh or stale against `fresh_for`.
  virtual std::vector<status::TelemetryView> views(std::chrono::milliseconds fresh_for) const = 0;
  /// The records as last persisted, read back from the file.
  virtual std::vector<status::Telemetry> persisted() const = 0;
  /// Writes the records to the node's telemetry file.
  virtual void persist() = 0;
  /// Asks for the local record to be refreshed and gossiped now.
  virtual void refresh() = 0;
};

/// What a Status page reads of the node, in one snapshot.
struct Diagnostics {
  /// When the node started, Unix milliseconds.
  uint64_t started_unix_ms = 0;
  /// Its public connectivity, when known.
  std::optional<reach::PublicConnectivityStatus> connectivity;
  /// Whether it accepts inbound connections, and why.
  reach::InboundState inbound;
  /// The client's counts.
  net::ClientStats client;
  /// The server's queues and timings.
  net::WorkStats work;
  /// Bytes in and out, per frame class.
  status::TrafficTotals traffic;
  /// Every supervised thread's state.
  std::vector<supervise::SupervisedThreadStatus> threads;
  /// Level 0: punches and relays tried and made, and the circuits through this node with their
  /// bytes.
  reach::PathStats paths;
  /// The hybrid clock: stamps not merged, overflows, ceiling raises off the roster loop, and each
  /// peer's largest offsets.
  contract::HlcStats hlc;
};

/// Who this node is: its id, its keys and custody, its counters, its certificates and scope.
/// Thread-safe.
class Identity {
 public:
  virtual ~Identity() = default;
  /// The node's id, kept across restarts in its state directory.
  virtual identity::NodeId id() const = 0;
  /// Fresh every process, so peers tell a restart from a reconnect.
  virtual identity::NodeId durability_epoch() const = 0;
  /// A key for the hosted system's own use, the same on every node of the cluster: derived from the
  /// cluster key under `label`, in secure memory.
  virtual identity::Secret derive_key(std::string_view label) const = 0;

  /// Whether the node has key custody (NodeConfig::custody).
  virtual bool has_custody() const = 0;
  /// The node's own public key (SubjectPublicKeyInfo); empty without custody.
  virtual identity::Bytes node_public_key() const = 0;
  /// A signature by the node's own key.
  /// @throws std::logic_error without custody.
  virtual identity::Bytes sign(std::span<const uint8_t> message) const = 0;
  /// A key for node-local use, derived from custody's seal root under `label` (Dagda's node-local
  /// spaces).
  /// @throws std::logic_error without custody.
  virtual identity::Secret derive_local(std::string_view label) const = 0;
  /// A node-local counter that only goes up, sealed beside custody: node-local rollback's first
  /// layer (identity/sealed_counters.h). Zero for a label never advanced. The counters refuse one
  /// directory restored alone, not a whole state restored together.
  /// @throws std::logic_error without custody.
  virtual uint64_t counter(std::string_view label) const = 0;
  /// Advances a counter, durable before it returns. Waits on the state device.
  /// @throws std::invalid_argument for a lower value, naming the label.
  /// @throws std::logic_error without custody.
  virtual void advance_counter(std::string_view label, uint64_t value) = 0;
  /// Advances several counters in one durable write. Throws as advance_counter().
  virtual void advance_counters(const std::map<std::string, uint64_t>& values) = 0;
  /// Notes a counter's advance, durable at the next flush, which the node makes within a second and
  /// at stop. Throws as advance_counter().
  virtual void note_counter(std::string_view label, uint64_t value) = 0;

  /// This node's certificate (wire version 2), when it has one.
  virtual std::optional<identity::Certificate> certificate() const = 0;
  /// The cluster's CA certificate, on a node with certificates.
  virtual std::optional<identity::Certificate> ca_certificate() const = 0;
  /// Applies a revocation the cluster's CA signed, here at once (the revoked node's sessions
  /// closed), and passes it to every active peer, each asked within the node's connect timeout.
  /// @throws std::invalid_argument for a revocation the CA did not sign.
  /// @throws std::logic_error on a node without certificates.
  virtual void revoke(const identity::Revocation& revocation) = 0;
  /// Every revocation held.
  virtual std::vector<identity::Revocation> revocations() const = 0;

  /// Installs the scope source (contract/scope.h) the directory provides (Lugh). Without one, this
  /// node and any node of the cluster may write, and nobody else. Never waits.
  virtual void install_scope_source(std::shared_ptr<const contract::ScopeSource> source) = 0;
  /// The check every hosted system makes before it stores a write: may `principal` write under
  /// `prefix`. Never waits.
  virtual contract::ScopeDecision verify(const contract::Principal& principal, std::string_view prefix) const = 0;
};

/// Memory, by work class: the node's retained-memory ledger as a hosted system holds it.
/// Thread-safe.
class Budget {
 public:
  virtual ~Budget() = default;
  /// Declares a unit of work the class must always be able to hold.
  /// @throws std::invalid_argument if the node's budget cannot hold one.
  virtual void declare(sched::WorkClass work, uint64_t unit_bytes) = 0;
  /// Holds `bytes` until the lease goes, charged to MemoryOwner::hosted_first. Waits on nothing.
  /// @return the lease; nothing when the class has no room now.
  virtual std::optional<sched::RetainedMemoryLedger::Lease> reserve(sched::WorkClass work, uint64_t bytes) = 0;
  /// As reserve(), charged to `owner`, the hosted system's own.
  /// @throws std::invalid_argument for an owner below MemoryOwner::hosted_first.
  virtual std::optional<sched::RetainedMemoryLedger::Lease> reserve(sched::WorkClass work, sched::MemoryOwner owner, uint64_t bytes) = 0;
  /// Waits until `bytes` may be held, as the node's ledger does, charged to `owner` as reserve().
  /// Waits on other holders and on the deadline.
  /// @param deadline on the steady clock (contract::Clock::now() plus what is left), not the node's
  ///   time(): a stepped clock's reading would put it far off either way.
  /// @param cancelled when set and wake() is called, the wait ends with nothing.
  /// @return the lease; nothing once the deadline passes or it is cancelled, and at once when the
  ///   class could never hold that much.
  /// @throws std::invalid_argument for no deadline (a default TimePoint).
  virtual std::optional<sched::RetainedMemoryLedger::Lease> acquire(sched::WorkClass work, sched::MemoryOwner owner, uint64_t bytes,
                                                                    contract::TimePoint deadline, std::atomic_bool* cancelled = nullptr) = 0;
  /// Wakes every acquire() to check its cancel flag again. Never waits.
  virtual void wake() = 0;
  /// The node's ledger as it stands, every owner's bytes among it.
  virtual sched::RetainedMemoryStats stats() const = 0;
};

/// Faults are values: raised, counted in Status, and handed to the sink the supervisor installs,
/// which restarts or disables. Thread-safe.
class Faults {
 public:
  /// Where faults go.
  using Sink = std::function<void(std::string reason)>;
  virtual ~Faults() = default;
  /// Installs the sink (the supervisor's).
  virtual void sink(Sink sink) = 0;
  /// Raises a fault, with why.
  virtual void raise(std::string reason) = 0;
};

/// The node's injected clocks, its executor, and a timer. Thread-safe.
class Time {
 public:
  virtual ~Time() = default;
  /// The node's steady time (source().now()).
  virtual contract::TimePoint now() const = 0;
  /// The node's wall time, Unix milliseconds.
  virtual uint64_t unix_ms() const = 0;
  /// The time source behind now(), for a component that takes one: a test's stepped source reaches
  /// the hosted system through here. Lives as long as the node.
  virtual const contract::TimeSource& source() const = 0;
  /// The node's io context's executor, for a hosted system's timers, strands and completions. Its
  /// handlers must not block.
  virtual boost::asio::any_io_executor executor() const = 0;
  /// Posts `fn` to the node's executor after `delay` of the node's time. Waits on nothing.
  virtual void after(std::chrono::milliseconds delay, std::function<void()> fn) = 0;
};

/// What a hosted system receives at start, and the only way it touches the network: the design's
/// nouns (Nodes, Routes, Channels, Identity, Budget, Faults, Time, Telemetry, Status).
///
/// Written as a specification a second implementation could satisfy: every operation states what it
/// waits on and its thread safety, every completion is posted to the executor the caller passed
/// (never called inline), no call waits without a deadline, and nothing that happened on more than
/// one node is reported as a bare boolean. Every noun is thread-safe and lives as long as the node.
///
/// @code
/// void start(dannan::Context& context) {
///   auto log = context.logger("mine");
///   context.readiness().set("mine.store", dannan::status::Health::ready);
///   context.channels().open("mine.events", dannan::net::Delivery::notify);
///   context.routes().bind(my_type, [](const auto& peer, auto frame, const dannan::net::Message& request) {
///     return dannan::net::Handled::processed({dannan::net::message::ok, request.payload, {}});
///   });
/// }
/// @endcode
class Context {
 public:
  virtual ~Context() = default;
  /// The contract version this node implements (context_version).
  virtual uint32_t version() const noexcept = 0;
  /// The cluster as this node knows it.
  virtual Nodes& nodes() = 0;
  /// The cluster as this node knows it, to read.
  virtual const Nodes& nodes() const = 0;
  /// Message types and calls.
  virtual Routes& routes() = 0;
  /// Channels and broadcasts.
  virtual Channels& channels() = 0;
  /// Who this node is.
  virtual Identity& identity() = 0;
  /// Who this node is, to read.
  virtual const Identity& identity() const = 0;
  /// Memory, by work class.
  virtual Budget& budget() = 0;
  /// Faults, as values.
  virtual Faults& faults() = 0;
  /// The node's clocks, executor and timer.
  virtual Time& time() = 0;
  /// Every node's telemetry.
  virtual Telemetry& telemetry() = 0;
  /// What the hosted system publishes, and its counters, for Status.
  virtual status::Surface& status() = 0;
  /// A logger on the node's one sink under `scope`, the hosted system's own short name ("dagda",
  /// "lugh.oauth"). There is no process-wide logger.
  virtual status::ScopedLogger logger(std::string scope) const = 0;
  /// Monotonic counts per kind; topology is Dannan's.
  virtual status::NodeEvents& events() = 0;
  /// The planes a hosted system declares and reports ready, and the node's health after startup
  /// (0.12.0): the checks a hosted system pushes, the drain, and on_change.
  virtual status::Readiness& readiness() = 0;
  /// The same, to read on a const Context (0.12.0; Macha: no const_cast to read health).
  virtual const status::Readiness& readiness() const = 0;
  /// When work of each class was last present; the node notes each request.
  virtual status::ActivityClocks& activity() = 0;
  /// Measurement probes, written to the node's observation log.
  virtual status::Observations& observations() = 0;
  /// Checks a work context against the waits an operation declares: the node's one guard, so its
  /// violations are counted together.
  virtual sched::WaitGuard& wait_guard() = 0;
  /// The node's thread bodies, and the hosted system's, under one record.
  virtual supervise::SupervisedThreads& threads() = 0;
  /// The same, to read on a const Context (0.12.0).
  virtual const supervise::SupervisedThreads& threads() const = 0;
  /// The node's pacing of loader and speculative work, one gate per class (P4): a hosted loop holds
  /// its class's gate, and every pool runs its tasks through it, so the class never takes more than
  /// its share however its work runs.
  virtual sched::ClassGates& gates() = 0;
  /// The node's worker pool, on the gates, started with the node. A hosted system may build a pool
  /// of its own beside it, on gates() and threads().
  virtual sched::WorkerPool& workers() = 0;
  /// The node's pool of large transfer buffers (0.11.0): take a buffer of an extent's size from it
  /// and give it back when done, so blocks are reused rather than freed through malloc. Dannan's
  /// sessions take and give the same way. The default keeps nothing, for a Context of a test's own.
  virtual std::shared_ptr<sched::BufferPool> buffers() { return std::make_shared<sched::BufferPool>(0, 0); }
  /// Leaderless per-key agreement over the cluster (0.9.0, TODO/2026-11-15-agreement-design.md): a
  /// hosted system serves its space's acceptors from a store of its own and changes registers from
  /// any node. Anything in the stack that can use agreement uses this, and builds no consensus of
  /// its own.
  virtual agree::Agreement& agreement() = 0;
  /// The node's hybrid logical clock (0.9.0): one per node, shared by every hosted system, its
  /// stamps never repeated across restarts. Its ceiling is sealed only with key custody; without
  /// custody it is a durable file, so a state directory restored alone from an old copy may reissue
  /// stamps.
  virtual contract::HlcClock& hlc() = 0;
  /// What a Status page reads, in one snapshot.
  virtual Diagnostics diagnostics() const = 0;
};

}  // namespace dannan
