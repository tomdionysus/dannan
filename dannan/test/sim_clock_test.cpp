//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The clock rules on whole nodes (TODO/2026-11-20-stack-plan.md, item 6): each node measures its
// offset to every peer from clock probes on the heartbeat; a node whose clock is past the bound
// against more than half of the agreed roster stops issuing stamps until it is back. One, two and
// three nodes by the same rule, with no special case by size.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

#include "dannan/contract/hlc.h"
#include "dannan/test/fixtures/sim/nodes.h"

namespace dannan::test::sim {
namespace {

using namespace std::chrono_literals;

bool joined(SimNodes& nodes) {
  for (size_t i = 0; i < nodes.size(); ++i)
    if (nodes.node(i).context().nodes().active().size() != nodes.size() || nodes.node(i).context().nodes().members().size() != nodes.size()) return false;
  return true;
}

// Whether node `i` has measured every peer.
bool measured(SimNodes& nodes, size_t i) {
  for (size_t j = 0; j < nodes.size(); ++j)
    if (j != i && !nodes.node(i).context().nodes().clock_offset(nodes.id(j))) return false;
  return true;
}

bool all_measured(SimNodes& nodes) {
  for (size_t i = 0; i < nodes.size(); ++i)
    if (!measured(nodes, i)) return false;
  return true;
}

bool in_bounds(SimNodes& nodes, size_t i) { return nodes.node(i).context().nodes().clock_in_bounds(); }

TEST(SimClock, EachNodeMeasuresEveryPeersOffset) {
  World world(11);
  SimNodes nodes(world, 3, {}, 300);
  ASSERT_TRUE(world.run_until([&] { return joined(nodes) && all_measured(nodes); }, 2'000'000));
  // The skew drawn is within 300 ms each way, so any two clocks are within 600 ms; a probe on the
  // world's network reads them within its round trip.
  for (size_t i = 0; i < 3; ++i)
    for (size_t j = 0; j < 3; ++j)
      if (i != j) EXPECT_LE(std::llabs(nodes.node(i).context().nodes().clock_offset(nodes.id(j))->count()), 700) << i << " to " << j;
  for (size_t i = 0; i < 3; ++i) EXPECT_TRUE(in_bounds(nodes, i));
}

TEST(SimClock, ANodeFarAheadOfTheMajorityStopsIssuingStampsAndComesBack) {
  World world(12);
  SimNodes nodes(world, 3);
  ASSERT_TRUE(world.run_until([&] { return joined(nodes) && all_measured(nodes); }, 2'000'000));
  nodes.step_clock(2, 60'000);
  ASSERT_TRUE(world.run_until([&] { return !in_bounds(nodes, 2); }, 2'000'000));
  EXPECT_THROW((void)nodes.node(2).context().hlc().now(), contract::ClockOutOfBounds);
  // Each of the others is off only node 2, one of three: within bounds.
  EXPECT_TRUE(in_bounds(nodes, 0));
  EXPECT_TRUE(in_bounds(nodes, 1));
  EXPECT_NO_THROW((void)nodes.node(0).context().hlc().now());
  // Node 2 judged itself from its own probes; node 0 sees the step at its own next probe of node
  // 2, which may come later (hosted TSan read the offset from before the step: 2 ms).
  ASSERT_TRUE(world.run_until([&] { return nodes.node(0).context().nodes().clock_offset(nodes.id(2))->count() >= 59'000; }, 2'000'000));
  EXPECT_TRUE(in_bounds(nodes, 0));
  // The others catch up: node 2 is back within bounds and issues again.
  nodes.step_clock(0, 60'000);
  nodes.step_clock(1, 60'000);
  ASSERT_TRUE(world.run_until([&] { return in_bounds(nodes, 2); }, 2'000'000));
  EXPECT_NO_THROW((void)nodes.node(2).context().hlc().now());
}

TEST(SimClock, TwoNodesFarApartAreBothWithinByTheSameRule) {
  World world(13);
  SimNodes nodes(world, 2);
  ASSERT_TRUE(world.run_until([&] { return joined(nodes) && all_measured(nodes); }, 2'000'000));
  nodes.step_clock(1, 60'000);
  ASSERT_TRUE(world.run_until([&] { return nodes.node(0).context().nodes().clock_offset(nodes.id(1))->count() > 59'000; }, 2'000'000));
  // A pair cannot reach a majority against either one: neither knows which clock is wrong.
  world.run_until([] { return false; }, 20'000);
  EXPECT_TRUE(in_bounds(nodes, 0));
  EXPECT_TRUE(in_bounds(nodes, 1));
}

TEST(SimClock, OneNodeIsWithinBoundsAlone) {
  World world(14);
  SimNodes nodes(world, 1);
  world.run_until([] { return false; }, 20'000);
  EXPECT_TRUE(in_bounds(nodes, 0));
  EXPECT_NO_THROW((void)nodes.node(0).context().hlc().now());
}

}  // namespace
}  // namespace dannan::test::sim
