//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Placement: the inventory's "Placement" table. Rendezvous for
// capacity-independent placement, capacity-aware placement over the 2^32
// shard space with failure domains preferred and fallbacks after, the R=1
// logical capacity, commit-replica order, and the extent candidate filter.
// That it decides as Macha does, key for key, is the interop suite's.

#include "dannan/placement/placement.h"

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <set>

namespace dannan::placement {
namespace {

using membership::NodeRecord;

NodeRecord node(uint8_t n, uint64_t capacity = 100, const std::string& domain = {}) {
  NodeRecord out;
  out.id.bytes[0] = n;
  out.capacity = capacity;
  out.failure_domain = domain;
  return out;
}

std::vector<uint8_t> key_of(uint32_t n) {
  std::vector<uint8_t> key(32);
  std::mt19937 random(n);
  for (auto& byte : key) byte = static_cast<uint8_t>(random());
  return key;
}

std::set<identity::NodeId> ids(const std::vector<NodeRecord>& nodes, size_t count) {
  std::set<identity::NodeId> out;
  for (size_t i = 0; i < count && i < nodes.size(); ++i) out.insert(nodes[i].id);
  return out;
}

TEST(Placement, RendezvousIsDeterministicAndPrefersDistinctDomains) {
  const std::vector<NodeRecord> nodes{node(1, 1, "a"), node(2, 1, "a"), node(3, 1, "b"), node(4, 1, "c")};
  for (uint32_t k = 0; k < 200; ++k) {
    const auto key = key_of(k);
    const auto first = rendezvous(key, nodes, 3);
    ASSERT_EQ(first.size(), 3u);
    EXPECT_EQ(ids(first, 3), ids(rendezvous(key, nodes, 3), 3));
    std::set<std::string> domains;
    for (const auto& n : first) domains.insert(n.failure_domain);
    EXPECT_EQ(domains.size(), 3u);
  }
  // More copies than domains: filled from the rest; never more than nodes.
  EXPECT_EQ(rendezvous(key_of(1), nodes, 4).size(), 4u);
  EXPECT_EQ(rendezvous(key_of(1), nodes, 9).size(), 4u);
}

// Every node appears once: the preferred owners, then the fallbacks.
TEST(Placement, CapacityPlacementListsEveryNodeOncePreferredFirst) {
  const std::vector<NodeRecord> nodes{node(1, 10, "a"), node(2, 20, "a"), node(3, 30, "b"), node(4, 40, "c"), node(5, 0, "d")};
  for (uint32_t k = 0; k < 300; ++k) {
    const auto placed = by_capacity(key_of(k), nodes, 3);
    ASSERT_EQ(placed.size(), nodes.size());
    EXPECT_EQ(ids(placed, placed.size()).size(), nodes.size());
    std::set<std::string> domains;
    for (size_t i = 0; i < 3; ++i) domains.insert(placed[i].failure_domain);
    EXPECT_EQ(domains.size(), 3u) << "key " << k;
  }
  EXPECT_TRUE(by_capacity(key_of(1), {}, 2).empty());
  EXPECT_TRUE(by_capacity(key_of(1), nodes, 0).empty());
}

// Capacity, not free space, sets the share: a node three times larger owns
// about three times the shards.
TEST(Placement, SharesFollowCapacity) {
  const std::vector<NodeRecord> nodes{node(1, 100), node(2, 300)};
  std::map<uint8_t, int> owned;
  for (uint32_t k = 0; k < 8000; ++k) ++owned[by_capacity(key_of(k), nodes, 1).front().id.bytes[0]];
  const double ratio = static_cast<double>(owned[2]) / owned[1];
  EXPECT_GT(ratio, 2.6);
  EXPECT_LT(ratio, 3.4);
}

// R=1: a newcomer takes only the shards it wins; no existing owner hands a
// shard to another existing owner.
TEST(Placement, ANewcomerOnlyTakesShardsItWins) {
  std::vector<NodeRecord> nodes{node(1, 100), node(2, 200), node(3, 150)};
  std::vector<identity::NodeId> before;
  for (uint32_t k = 0; k < 3000; ++k) before.push_back(by_capacity(key_of(k), nodes, 1).front().id);
  nodes.push_back(node(4, 120));
  for (uint32_t k = 0; k < 3000; ++k) {
    const auto after = by_capacity(key_of(k), nodes, 1).front().id;
    EXPECT_TRUE(after == before[k] || after == node(4).id) << "key " << k;
  }
}

// What a write can count on storing as R distinct copies: R=1 aggregates
// every node; R=2 is held back by the largest node, which can hold only one
// copy of anything.
TEST(Placement, LogicalCapacityHonoursTheCopies) {
  EXPECT_EQ(logical_capacity({node(1, 10), node(2, 20), node(3, 30)}, 1), 60u);
  EXPECT_EQ(logical_capacity({node(1, 1), node(2, 1), node(3, 10)}, 2), 2u);
  EXPECT_EQ(logical_capacity({node(1, 10), node(2, 10)}, 2), 10u);
  EXPECT_EQ(logical_capacity({}, 2), 0u);
  EXPECT_EQ(logical_capacity({node(1, 10)}, 0), 0u);
  // Two domains for three copies: counted by node.
  EXPECT_EQ(logical_capacity({node(1, 10, "a"), node(2, 10, "a"), node(3, 10, "b")}, 3), 10u);
}

TEST(Placement, CommitReplicasGoLocalFirstThenByRoundTrip) {
  const std::vector<NodeRecord> replicas{node(1), node(2), node(3), node(4)};
  const auto ordered = order_commit_replicas(replicas, node(3).id, [](const identity::NodeId& id) -> std::optional<std::chrono::milliseconds> {
    if (id.bytes[0] == 1) return std::chrono::milliseconds(40);
    if (id.bytes[0] == 4) return std::chrono::milliseconds(5);
    return std::nullopt;
  });
  std::vector<uint8_t> order;
  for (const auto& n : ordered) order.push_back(n.id.bytes[0]);
  EXPECT_EQ(order, (std::vector<uint8_t>{3, 4, 1, 2}));
}

TEST(Placement, ANodeWithoutExtentsIsNeverACandidate) {
  auto edge = node(2);
  edge.flags = membership::flags_for(true, false);
  const auto candidates = extent_candidates({node(1), edge, node(3)});
  ASSERT_EQ(candidates.size(), 2u);
  for (const auto& n : candidates) EXPECT_NE(n.id, edge.id);
}

}  // namespace
}  // namespace dannan::placement
