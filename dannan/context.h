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
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/client.h"
#include "dannan/net/scheduler.h"
#include "dannan/placement/placement.h"
#include "dannan/sched/retained_memory.h"
#include "dannan/status/events.h"
#include "dannan/status/surface.h"

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

// The cluster as this node knows it. Reads of local state; wait on nothing.
class Nodes {
 public:
  virtual ~Nodes() = default;
  virtual membership::NodeRecord self() const = 0;
  // Every known node, self first; and those heard of within dead_after.
  virtual std::vector<membership::NodeRecord> roster() const = 0;
  virtual std::vector<membership::NodeRecord> active() const = 0;
  // Authenticated directly within dead_after; gossip never counts.
  virtual bool directly_reachable(const identity::NodeId& node) const = 0;
  virtual bool inbound_capable(const identity::NodeId& node) const = 0;
  // The smoothed CONTROL round trip, from health pings only.
  virtual std::optional<std::chrono::milliseconds> latency(const identity::NodeId& node) const = 0;
  // Ordered candidates for `key` over the active roster: the first `copies`
  // preferred, the rest fallbacks; `extents_only` keeps nodes that host
  // extents.
  virtual std::vector<membership::NodeRecord> placement(std::span<const uint8_t> key, size_t copies, bool extents_only = false,
                                                        const placement::Salts& salts = {}) const = 0;
};

// Message types and calls. A hosted system registers its own types before
// the node starts; Dannan's core types are not its to take.
class Routes {
 public:
  using Done = net::Client::Done;
  virtual ~Routes() = default;
  // Throws std::logic_error after start or for a core type, and
  // std::invalid_argument for a number taken or retired.
  virtual void add(net::MessageType type, net::Registration registration, net::RequestHandler handler) = 0;
  // Calls `peer`, dialling if need be; `done` is posted to `executor` once:
  // the reply, or why none came, at the latest when `deadline` passes.
  // Waits on nothing.
  virtual void call(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                    boost::asio::any_io_executor executor, Done done) = 0;
  virtual void call(const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                    boost::asio::any_io_executor executor, Done done) = 0;
};

// Channels v1: a notification to every peer over CONTROL, best effort and
// never waiting on a peer; bulk transfer is a Routes call on a DATA-lane
// type.
class Channels {
 public:
  virtual ~Channels() = default;
  // How many peers' queues took it. Waits for the route table, never a peer.
  virtual size_t notify(const net::Message& message, sched::FrameType frame) = 0;
};

// Who this node is.
class Identity {
 public:
  virtual ~Identity() = default;
  virtual identity::NodeId id() const = 0;
  // Fresh every process: peers tell a restart from a reconnect.
  virtual identity::NodeId durability_epoch() const = 0;
  // A key for the hosted system's own use, derived from the cluster key
  // under its label.
  virtual std::array<uint8_t, 32> derive_key(std::string_view label) const = 0;
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

// The node's injected clocks, and a timer.
class Time {
 public:
  virtual ~Time() = default;
  virtual contract::TimePoint now() const = 0;
  virtual uint64_t unix_ms() const = 0;
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
  virtual const Identity& identity() const = 0;
  virtual Budget& budget() = 0;
  virtual Faults& faults() = 0;
  virtual Time& time() = 0;
  // What the hosted system publishes, and its counters, for Status.
  virtual status::Surface& status() = 0;
  // Monotonic counts per kind; topology is Dannan's.
  virtual status::NodeEvents& events() = 0;
};

}  // namespace dannan
