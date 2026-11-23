//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Simulated nodes in a test (docs/guide/clocks.md): whole nodes in one seeded world, on its time
// and its network, with faults. A hosted system's type registered in prepare, before each node
// starts; a call across the world; and the same seed replaying the same run, so a failure found
// once is found every time. A GoogleTest program.

#include <gtest/gtest.h>

#include <optional>

#include "dannan/test/fixtures/sim/nodes.h"

namespace {

using namespace std::chrono_literals;
using dannan::test::sim::NetworkFaults;
using dannan::test::sim::SimNodes;
using dannan::test::sim::World;

constexpr dannan::net::MessageType echo{1500};

bool everyone_knows_everyone(SimNodes& nodes) {
  for (size_t i = 0; i < nodes.size(); ++i) {
    size_t known = 0;
    for (const auto& record : nodes.node(i).context().nodes().active())
      for (size_t j = 0; j < nodes.size(); ++j)
        if (record.id == nodes.id(j)) ++known;
    if (known != nodes.size()) return false;
  }
  return true;
}

// The world's time at which three nodes, on a network that drops and delays, all know each other,
// and a call across it is answered.
uint64_t run(uint64_t seed) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 20;   // one message in twenty lost
  faults.max_delay_ms = 30;  // each delayed up to 30 ms of the world's time
  SimNodes nodes(world, 3, faults, 0, 0, /*started=*/false);
  // Registered on every node before it starts, as on a real node.
  nodes.prepare = [](size_t, dannan::Node& node) {
    node.context().routes().add(echo, {"echo", dannan::net::Lanes::control, dannan::net::Frames::any, false},
                                [](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
                                  return dannan::net::Handled::processed({dannan::net::message::ok, request.payload, {}});
                                });
  };
  nodes.start_all();
  EXPECT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
  dannan::membership::NodeRecord peer;
  for (const auto& record : nodes.node(0).context().nodes().active())
    if (record.id == nodes.id(2)) peer = record;
  std::optional<dannan::net::CallOutcome> answer;
  (void)nodes.node(0).context().routes().call(peer, echo, dannan::net::Bytes{42}, dannan::sched::FrameType::control, 5s, world.io.get_executor(),
                                              [&](dannan::net::CallOutcome outcome) { answer = std::move(outcome); });
  EXPECT_TRUE(world.run_until([&] { return answer.has_value(); }, 2'000'000));
  EXPECT_TRUE(answer && answer->reply && answer->reply->payload == dannan::net::Bytes{42});
  return world.now_ms();
}

TEST(SimulatedExample, ThreeNodesOnAFaultyNetworkAnswerACall) { EXPECT_GT(run(7), 0u); }

// The same seed, the same run: the world's every event in the same order, to the millisecond.
TEST(SimulatedExample, TheSameSeedReplaysTheSameRun) { EXPECT_EQ(run(11), run(11)); }

}  // namespace
