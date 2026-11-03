//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/membership/node_record.h"

// Where things live: pure functions of a key and a roster, so every node
// that knows the same roster computes the same owners without a word.
namespace dannan::placement {

using membership::NodeRecord;

// A key's first 32 bits name its shard (a key shorter than four bytes is
// hashed first). Capacity moves ownership boundaries in this space; free
// space never feeds back into it.
using Shard = uint32_t;
inline constexpr uint64_t shard_count = uint64_t{1} << 32;

// How a hosted system places: the salts capacity placement hashes under,
// which the system names (there is no default), and the advert holding
// each node's capacity as a u64 (NodeRecord::advert_u64); a node without
// it has none.
struct Scheme {
  std::string fallback_salt;
  std::string extra_domain_salt;
  std::string capacity_advert;
};

// A node's capacity under `advert`; zero without it.
uint64_t capacity_of(const NodeRecord& node, const std::string& advert);

// Plain rendezvous (highest random weight) over SHA-256(key, node id), for
// placement that ignores capacity: up to `count` nodes, distinct failure
// domains first, in rendezvous order.
std::vector<NodeRecord> rendezvous(std::span<const uint8_t> key, const std::vector<NodeRecord>& nodes, size_t count);

// Capacity-aware placement: the first `count` (at most every node) are the
// preferred owners, distinct failure domains first, each node's share of
// the shard space in proportion to its domain's capacity; then every other
// node, as deterministic fallbacks a full or offline owner spills to. With
// count 1, weighted rendezvous over the shard: a newcomer takes only the
// shards it wins. Empty for no nodes or no copies. Throws
// std::invalid_argument for a scheme without both salts.
std::vector<NodeRecord> by_capacity(std::span<const uint8_t> key, const std::vector<NodeRecord>& nodes, size_t count, const Scheme& scheme);

// The most logical bytes storable as `replicas` copies on distinct nodes,
// distinct failure domains where there are enough: what a filesystem
// reports, not the sum over R.
uint64_t logical_capacity(const std::vector<NodeRecord>& nodes, size_t replicas, const std::string& capacity_advert);

// A critical-path fan-out's order: the local replica first, then peers by
// measured round trip, unmeasured last; stable otherwise.
std::vector<NodeRecord> order_commit_replicas(std::vector<NodeRecord> replicas, const identity::NodeId& local,
                                              const std::function<std::optional<std::chrono::milliseconds>(const identity::NodeId&)>& latency);

// The nodes advertising `key`: how a hosted system narrows placement to the
// nodes that serve it (a store's "hosts extents").
std::vector<NodeRecord> with_advert(const std::vector<NodeRecord>& nodes, const std::string& key);

}  // namespace dannan::placement
