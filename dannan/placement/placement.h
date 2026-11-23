//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// Where things live: pure functions of a key and a roster.

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/membership/node_record.h"

/// Placement: pure functions of a key and a roster, so every node that knows the same roster
/// computes the same owners without a word exchanged. Context::placement() applies them to the
/// active roster.
namespace dannan::placement {

/// A node as the roster has it.
using membership::NodeRecord;

/// A key's shard: its first 32 bits (a key shorter than four bytes is hashed first). Capacity
/// moves ownership boundaries in this space; free space never feeds back into it.
using Shard = uint32_t;
/// How many shards there are: 2^32.
inline constexpr uint64_t shard_count = uint64_t{1} << 32;

/// How a hosted system places: the salts capacity placement hashes under, which the system names
/// (there is no default, so two systems never place alike by accident), and the advert holding
/// each node's capacity.
struct Scheme {
  /// The salt the fallback order is hashed under.
  std::string fallback_salt;
  /// The salt that picks extra copies when there are fewer failure domains than copies.
  std::string extra_domain_salt;
  /// The advert holding each node's capacity as a u64 (NodeRecord::advert_u64); a node without it
  /// has none.
  std::string capacity_advert;
  /// Bulk placement that knows level 0: a relay-only node still holds data, but is never a
  /// preferred owner while enough other nodes can be; it leads the fallbacks. Off by default, so
  /// level 0 does not leak into placement unless asked.
  bool demote_relay_only = false;
  /// From 0.12.0, nodes advertising draining, then not_ready (status::health_of), follow the ready
  /// and degraded ones as fallbacks, never removed, while there are at least `count` of those. So
  /// a draining node stops being chosen for new copies but stays a holder, and a node that comes
  /// back takes its own shards back. Set this to read no health, for a system that judges it
  /// itself.
  bool ignore_health = false;
};

/// A node's capacity under `advert`; zero without it.
uint64_t capacity_of(const NodeRecord& node, const std::string& advert);

/// Plain rendezvous (highest random weight) over SHA-256(key, node id), for placement that ignores
/// capacity.
/// @param key what is placed.
/// @param nodes the roster.
/// @param count how many nodes to name.
/// @param ignore_health false (the default from 0.12.0): nodes advertising draining or not_ready
///   are passed over while at least `count` others are ready or degraded.
/// @return up to `count` nodes, distinct failure domains first, in rendezvous order.
std::vector<NodeRecord> rendezvous(std::span<const uint8_t> key, const std::vector<NodeRecord>& nodes, size_t count, bool ignore_health = false);

/// Capacity-aware placement.
///
/// The first `count` (at most every node) are the preferred owners, distinct failure domains first,
/// each node's share of the shard space in proportion to its domain's capacity; then every other
/// node, as deterministic fallbacks a full or offline owner spills to. With count 1, weighted
/// rendezvous over the shard: a newcomer takes only the shards it wins. Health and relay-only
/// demotion are as the Scheme says.
/// @return every node, preferred owners first; empty for no nodes or no copies.
/// @throws std::invalid_argument for a scheme without both salts.
std::vector<NodeRecord> by_capacity(std::span<const uint8_t> key, const std::vector<NodeRecord>& nodes, size_t count, const Scheme& scheme);

/// The most logical bytes storable as `replicas` copies on distinct nodes, distinct failure domains
/// where there are enough: what a filesystem reports, not the sum over the replicas.
uint64_t logical_capacity(const std::vector<NodeRecord>& nodes, size_t replicas, const std::string& capacity_advert);

/// A critical-path fan-out's order: the local replica first, then peers by measured round trip,
/// unmeasured last; stable otherwise.
/// @param latency each peer's measured round trip, or nothing.
std::vector<NodeRecord> order_commit_replicas(std::vector<NodeRecord> replicas, const identity::NodeId& local,
                                              const std::function<std::optional<std::chrono::milliseconds>(const identity::NodeId&)>& latency);

/// The nodes advertising `key`: how a hosted system narrows placement to the nodes that serve it
/// (a store's "hosts extents").
std::vector<NodeRecord> with_advert(const std::vector<NodeRecord>& nodes, const std::string& key);

}  // namespace dannan::placement
