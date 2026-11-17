//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Whole nodes in the world (the harness's step 3 part 4, piece 2b): real dannan::Node
// membership on the world's transport, time and schedule. Nodes join and gossip until every
// one knows every other; a seed replays exactly; a clean split is seen from both sides and
// healed (Mimir's ask); a node restarted keeps its identity, or with its state wiped comes
// back as a new one that the others learn.

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "dannan/test/fixtures/sim/nodes.h"

namespace dannan::test::sim {
namespace {

bool knows(Node& node, const identity::NodeId& id) {
  const auto roster = node.context().nodes().active();
  return std::any_of(roster.begin(), roster.end(), [&](const membership::NodeRecord& record) { return record.id == id; });
}

bool everyone_knows_everyone(SimNodes& nodes) {
  for (size_t i = 0; i < nodes.size(); ++i)
    for (size_t j = 0; j < nodes.size(); ++j)
      if (!nodes.up(i) || !nodes.up(j) || !knows(nodes.node(i), nodes.id(j))) return false;
  return true;
}

TEST(SimNodes, FiveNodesJoinAndEveryOneKnowsEveryOther) {
  World world(1);
  NetworkFaults faults;
  faults.drop_one_in = 20;
  faults.max_delay_ms = 20;
  SimNodes nodes(world, 5, faults);
  ASSERT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
}

// The world's time at which everyone first knows everyone: the same for the same seed.
uint64_t joined_at(uint64_t seed) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 10;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  SimNodes nodes(world, 5, faults, 200, 100);
  if (!world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000)) return 0;
  return world.now_ms();
}

TEST(SimNodes, ASeedReplaysExactly) {
  const auto first = joined_at(7);
  ASSERT_NE(first, 0u);
  EXPECT_EQ(joined_at(7), first);
}

// Mimir's third ask: a clean split, each side seeing the other leave within dead_after, and a
// heal at a chosen tick after which everyone knows everyone again.
TEST(SimNodes, ACleanSplitIsSeenFromBothSidesAndHealed) {
  World world(3);
  SimNodes nodes(world, 5);
  ASSERT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
  nodes.network().partition({{0, 1, 2}, {3, 4}});
  const auto apart = [&] {
    for (const size_t a : {0, 1, 2})
      for (const size_t b : {3, 4})
        if (knows(nodes.node(a), nodes.id(b)) || knows(nodes.node(b), nodes.id(a))) return false;
    return true;
  };
  ASSERT_TRUE(world.run_until(apart, 2'000'000));
  // The sides still know their own.
  EXPECT_TRUE(knows(nodes.node(0), nodes.id(2)));
  EXPECT_TRUE(knows(nodes.node(3), nodes.id(4)));
  const auto heal_at = world.now_ms() + 1000;
  world.after(1000, [&] { nodes.network().heal(); });
  ASSERT_TRUE(world.run_until([&] { return world.now_ms() >= heal_at && everyone_knows_everyone(nodes); }, 2'000'000));
}

// Mimir's second ask: a node restarted with its state keeps its identity; restarted wiped it is a
// new node, which the others learn.
TEST(SimNodes, ANodeRestartedKeepsItsIdentityOrWipedComesBackAsANewOne) {
  World world(5);
  SimNodes nodes(world, 3);
  ASSERT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
  const auto before = nodes.id(2);
  nodes.restart(2);
  EXPECT_EQ(nodes.id(2), before);
  ASSERT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
  nodes.restart(2, true);
  EXPECT_NE(nodes.id(2), before);
  ASSERT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
}

}  // namespace
}  // namespace dannan::test::sim
