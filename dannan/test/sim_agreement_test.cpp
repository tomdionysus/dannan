//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Agreement under the simulation harness (TODO/2026-11-15-simulation-harness.md): five
// nodes running Dannan's real Agreement increment one counter from every side while the
// world loses, delays, duplicates and one-way-blocks messages and crashes and restarts
// nodes. The history is checked for linearizability; and the same checker must refuse a
// protocol broken on purpose (an acceptor that votes after losing its state), or it has
// never been shown to refuse anything.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/agreement.h"

namespace dannan::test::sim {
namespace {

using namespace std::chrono_literals;

agree::Bytes encode(uint64_t value) {
  agree::Bytes out(8);
  for (int i = 0; i < 8; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (56 - 8 * i));
  return out;
}

uint64_t decode(const std::optional<agree::Bytes>& bytes) {
  if (!bytes || bytes->size() != 8) return 0;
  uint64_t out = 0;
  for (const auto byte : *bytes) out = (out << 8) | byte;
  return out;
}

// An increment: the counter's next value, which is also the result.
agree::Change increment() {
  return [](std::span<const std::optional<agree::Bytes>> current) {
    const auto next = decode(current[0]) + 1;
    return agree::Changed{{encode(next)}, encode(next)};
  };
}

struct Op {
  uint64_t invoked = 0;
  uint64_t completed = 0;
  bool answered = false;
  bool committed = false;
  uint64_t result = 0;
};

// A counter's history is linearizable when every committed increment returned a distinct
// value, a value no later than one invoked after it completed, and the final value is
// explained by what committed and what may have (an increment with no committed answer may
// or may not have taken effect).
std::optional<std::string> check(const std::vector<Op>& history, uint64_t final_value) {
  std::set<uint64_t> seen;
  size_t committed = 0;
  size_t unknown = 0;
  for (const auto& op : history) {
    if (!op.committed) {
      ++unknown;
      continue;
    }
    ++committed;
    if (!seen.insert(op.result).second) return "two increments both returned " + std::to_string(op.result);
    if (op.result > final_value) return "an increment returned " + std::to_string(op.result) + " above the final " + std::to_string(final_value);
  }
  if (final_value < committed) return "the final value " + std::to_string(final_value) + " is below the " + std::to_string(committed) + " committed";
  if (final_value > committed + unknown) return "the final value " + std::to_string(final_value) + " is above everything that may have taken effect";
  for (const auto& a : history)
    for (const auto& b : history)
      if (a.committed && b.committed && a.completed < b.invoked && !(a.result < b.result))
        return "real time: " + std::to_string(a.result) + " completed before " + std::to_string(b.result) + " was invoked";
  return std::nullopt;
}

struct Run {
  std::optional<std::string> violation;
  // The run itself did not finish: the harness's failure, not a history the checker refused.
  bool unsettled = false;
  size_t committed = 0;
};

// One seeded run. `broken`: a crashed node may restart with its state lost and vote at
// once, which the design forbids.
Run simulate(uint64_t seed, bool broken) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 8;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  Cluster cluster(world, 5, faults);
  std::vector<Op> history(40);
  // Clients on every node, at random times over two simulated seconds.
  for (size_t n = 0; n < history.size(); ++n) {
    world.after(world.random.below(2000), [&, n] {
      size_t at = world.random.below(cluster.size());
      for (size_t tries = 0; tries < cluster.size() && !cluster.up(at); ++tries) at = (at + 1) % cluster.size();
      if (!cluster.up(at)) return;
      history[n].invoked = world.now_ms();
      cluster.agreement(at).change(Cluster::space, {agree::Name{'c'}}, cluster.ids(), 1, increment(), {}, 3000ms, world.io.get_executor(),
                                   [&, n](agree::Outcome outcome) {
                                     auto& op = history[n];
                                     op.completed = world.now_ms();
                                     op.answered = true;
                                     op.committed = outcome.committed;
                                     if (outcome.committed) op.result = decode(outcome.result);
                                   });
    });
  }
  // Faults over the same time: one-way partitions, crashes and restarts.
  for (int f = 0; f < 12; ++f) {
    world.after(world.random.below(2000), [&] {
      const auto a = world.random.below(cluster.size());
      const auto b = world.random.below(cluster.size());
      switch (world.random.below(4)) {
        case 0:
          if (a != b) cluster.block(a, b);
          break;
        case 1:
          cluster.unblock_all();
          break;
        case 2:
          if (cluster.up(a)) cluster.crash(a);
          break;
        default:
          if (!cluster.up(a)) {
            // The broken protocol wipes and votes at once; the correct one keeps its state.
            if (broken) {
              cluster.restart(a, true, true);
            } else {
              cluster.restart(a);
            }
          }
          break;
      }
    });
  }
  // Then heal: every node back, every link open, until every operation is answered.
  world.after(2500, [&] {
    cluster.unblock_all();
    cluster.faults().drop_one_in = 0;
    cluster.faults().duplicate_one_in = 0;
    for (size_t i = 0; i < cluster.size(); ++i)
      if (!cluster.up(i)) cluster.restart(i, broken, true);
  });
  const auto all_answered = [&] { return std::all_of(history.begin(), history.end(), [](const Op& op) { return op.answered || op.invoked == 0; }); };
  Run run;
  // Past the heal, which is always in the queue, and every operation answered.
  if (!world.run_until([&] { return world.now_ms() >= 1'002'500 && all_answered(); }, 2'000'000)) {
    run.unsettled = true;
    run.violation = "the run did not settle";
    return run;
  }
  // The final value, by a strict read once healed.
  std::optional<agree::Outcome> read;
  cluster.agreement(0).read(Cluster::space, {agree::Name{'c'}}, cluster.ids(), 1, {}, 5000ms, world.io.get_executor(),
                            [&](agree::Outcome outcome) { read = std::move(outcome); });
  world.run_until([&] { return read.has_value(); }, 2'000'000);
  if (!read || !read->committed) {
    run.unsettled = true;
    run.violation = "the final read did not commit";
    return run;
  }
  std::vector<Op> invoked;
  for (const auto& op : history)
    if (op.invoked) invoked.push_back(op);
  run.committed = static_cast<size_t>(std::count_if(invoked.begin(), invoked.end(), [](const Op& op) { return op.committed; }));
  run.violation = check(invoked, decode(read->values[0]));
  return run;
}

uint64_t seeds(uint64_t fallback) {
  const char* set = std::getenv("DANNAN_SIM_SEEDS");
  return set && *set ? std::strtoull(set, nullptr, 10) : fallback;
}

// The protocol as designed: whatever the network and crashes do, every seed's history is
// linearizable, and work gets done.
TEST(Simulation, AgreementStaysLinearizableUnderFaults) {
  size_t committed = 0;
  for (uint64_t seed = 1; seed <= seeds(150); ++seed) {
    const auto run = simulate(seed, false);
    ASSERT_FALSE(run.violation) << "seed " << seed << ": " << *run.violation;
    committed += run.committed;
  }
  EXPECT_GT(committed, 0u);
}

// The checker refuses a protocol broken on purpose (Miach's point: a verifier that has
// never refused anything is unverified). An acceptor that votes after losing its state can
// let a value already chosen be forgotten; some seed must show it.
TEST(Simulation, TheCheckerCatchesAnAcceptorThatVotesAfterLosingItsState) {
  std::optional<std::string> caught;
  uint64_t at = 0;
  for (uint64_t seed = 1; seed <= 400 && !caught; ++seed) {
    const auto run = simulate(seed, true);
    // A refusal by the checker, not a run that merely failed to finish.
    if (run.violation && !run.unsettled) {
      caught = run.violation;
      at = seed;
    }
  }
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checker is unproven";
  SUCCEED() << "seed " << at << ": " << *caught;
}

// A seed replays exactly.
TEST(Simulation, ASeedReplaysExactly) {
  const auto first = simulate(42, false);
  const auto again = simulate(42, false);
  EXPECT_EQ(first.committed, again.committed);
  EXPECT_EQ(first.violation, again.violation);
}

}  // namespace
}  // namespace dannan::test::sim
