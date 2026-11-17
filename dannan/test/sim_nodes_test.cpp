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
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/history.h"
#include "dannan/test/fixtures/sim/linearizable.h"
#include "dannan/test/fixtures/sim/models.h"
#include "dannan/test/fixtures/sim/nodes.h"

namespace dannan::test::sim {
namespace {

using namespace std::chrono_literals;

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
  for (uint64_t seed = 1; seed <= 10; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));

    World world(seed);
    SimNodes nodes(world, 5);
    ASSERT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
    // Settled first: a second of heartbeats, so every node has reached every other itself, not only
    // heard of it through a seed's gossip. A peer known only through gossip when a split comes may
    // be lost on its own side before it is reached: a pass reaches its peers in turn, and each one
    // across the split waits out the exchange budget.
    const auto settled = world.now_ms() + 1000;
    ASSERT_TRUE(world.run_until([&] { return world.now_ms() >= settled; }, 2'000'000));
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
  nodes.restart(2, Wipe::identity);
  EXPECT_NE(nodes.id(2), before);
  ASSERT_TRUE(world.run_until([&] { return everyone_knows_everyone(nodes); }, 2'000'000));
}

// Agreement over real membership in the world: five nodes serve one space from their own stores
// and advertise its capability through real gossip; a counter is incremented from every side
// while messages are lost, duplicated and delayed and nodes restart, keeping their stores, or as
// the same node with its store lost (the restart rule: it votes only once caught up). The history
// is linearizable.
struct Counting {
  uint64_t invoked = 0;
  uint64_t answered = 0;
  bool committed = false;
  uint64_t result = 0;
};

uint64_t number(const std::optional<agree::Bytes>& bytes) {
  if (!bytes || bytes->size() != 8) return 0;
  uint64_t out = 0;
  for (const auto byte : *bytes) out = (out << 8) | byte;
  return out;
}

// `split`: the cluster is split 3 and 2 while the increments run, and each change names, by turns,
// the genesis set and the set its own node's roster gives (the nodes it still sees, at a
// version above genesis): two rosters naming different sets, from real membership. `trace`, if
// given, records what each operation was told and when, and the final read.
std::optional<std::string> agreement_over_nodes(uint64_t seed, bool split = false, std::string* trace = nullptr) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 12;
  faults.duplicate_one_in = 12;
  faults.max_delay_ms = 20;
  SimNodes nodes(world, 5, faults, 50, 50, false);
  std::vector<std::shared_ptr<Store>> stores(nodes.size());
  for (auto& store : stores) store = std::make_shared<Store>(true);
  nodes.configure = [&](size_t, NodeConfig& config) {
    config.agreement_genesis = nodes.ids();
    config.agreement_slots = 4;
  };
  const std::string space = "sim";
  nodes.host = [&](size_t i, Node& node, bool lost) {
    if (lost) {
      stores[i] = std::make_shared<Store>(true);
      stores[i]->set_ready(false);
    }
    node.context().agreement().serve(space, space, stores[i]);
    node.context().nodes().advertise(space, {});
    if (!lost) return;
    // Caught up by a strict read at a majority of the others, then voting.
    // Held weakly by itself and strongly by the attempt in flight, so it ends with the last one.
    auto again = std::make_shared<std::function<void()>>();
    *again = [&, i, store = stores[i], weak = std::weak_ptr(again)] {
      const auto self = weak.lock();
      if (!self || !nodes.up(i) || stores[i] != store) return;
      nodes.node(i).context().agreement().catch_up(space, 0, {agree::Name{'c'}}, nodes.ids(), 0, 1000ms, world.io.get_executor(),
                                                   [&world, store, self](agree::Outcome outcome) {
                                                     if (outcome.committed) return store->set_ready(true);
                                                     world.after(100, [self] { (*self)(); });
                                                   });
    };
    (*again)();
  };
  nodes.start_all();
  const auto ready = [&] {
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (!nodes.up(i)) return false;
      const auto roster = nodes.node(i).context().nodes().active();
      for (size_t j = 0; j < nodes.size(); ++j)
        if (std::none_of(roster.begin(), roster.end(),
                         [&](const membership::NodeRecord& record) { return record.id == nodes.id(j) && record.adverts.contains(space); }))
          return false;
    }
    return true;
  };
  if (!world.run_until(ready, 2'000'000)) return "the nodes never all advertised the space";
  // Settled first, as for a split of membership alone: every node has reached every other itself.
  const auto calm = world.now_ms() + 1000;
  world.run_until([&] { return world.now_ms() >= calm; }, 2'000'000);
  std::vector<Counting> ops(30);
  const auto start = world.now_ms();
  for (size_t n = 0; n < ops.size(); ++n)
    world.after(world.random.below(1500), [&, n] {
      size_t at = world.random.below(nodes.size());
      for (size_t tries = 0; tries < nodes.size() && !nodes.up(at); ++tries) at = (at + 1) % nodes.size();
      if (!nodes.up(at)) return;
      ops[n].invoked = world.now_ms();
      // By turns, under a split: the set this node's own roster gives.
      std::vector<identity::NodeId> replicas = nodes.ids();
      uint64_t version = 0;
      if (split && n % 2) {
        replicas.clear();
        for (const auto& record : nodes.node(at).context().nodes().active()) replicas.push_back(record.id);
        version = 1;
      }
      nodes.node(at).context().agreement().change(
          space, 0, {agree::Name{'c'}}, replicas, version,
          [](std::span<const std::optional<agree::Bytes>> current) {
            const auto next = number(current[0]) + 1;
            const auto bytes = CounterModel::number(next);
            return agree::Changed{{agree::Bytes(bytes.begin(), bytes.end())}, agree::Bytes(bytes.begin(), bytes.end())};
          },
          {}, 3000ms, world.io.get_executor(),
          [&, n](agree::Outcome outcome) {
            ops[n].answered = world.now_ms();
            ops[n].committed = outcome.committed;
            if (outcome.committed) ops[n].result = number(outcome.result);
          });
    });
  if (split) {
    nodes.network().partition({{0, 1, 2}, {3, 4}});
    world.after(1200, [&] { nodes.network().heal(); });
  } else {
    // Restarts: node 1 keeping its store, node 3 as itself with its store lost.
    world.after(400, [&] { nodes.restart(1); });
    world.after(900, [&] { nodes.restart(3, Wipe::store); });
  }
  world.after(1600, [&] { nodes.network().faults() = NetworkFaults{}; });
  const auto settled = [&] {
    if (world.now_ms() < start + 1600) return false;
    for (const auto& op : ops)
      if (op.invoked && !op.answered) return false;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (!stores[i]->ready()) return false;
    return true;
  };
  if (!world.run_until(settled, 4'000'000)) return "the run did not settle";
  std::optional<agree::Outcome> read;
  nodes.node(0).context().agreement().read(space, 0, {agree::Name{'c'}}, nodes.ids(), 0, {}, 5000ms, world.io.get_executor(),
                                           [&](agree::Outcome outcome) { read = std::move(outcome); });
  world.run_until([&] { return read.has_value(); }, 2'000'000);
  if (!read || !read->committed) return "the final read did not commit";
  if (trace) {
    for (const auto& op : ops)
      *trace += std::to_string(op.invoked) + ":" + std::to_string(op.answered) + ":" + std::to_string(op.committed) + ":" + std::to_string(op.result) + " ";
    *trace += "read " + std::to_string(number(read->values[0])) + " at " + std::to_string(world.now_ms());
  }
  History history;
  uint64_t last = 0;
  size_t committed = 0;
  for (size_t i = 0; i < ops.size(); ++i) {
    if (!ops[i].invoked) continue;
    const auto at = history.invoke(i, {"c"}, Bytes{'i'}, ops[i].invoked);
    if (ops[i].committed) {
      history.answer(at, CounterModel::number(ops[i].result), ops[i].answered);
      ++committed;
    }
    last = std::max({last, ops[i].invoked, ops[i].answered});
  }
  const auto final_read = history.invoke(ops.size(), {"c"}, Bytes{'r'}, last + 1);
  history.answer(final_read, CounterModel::number(number(read->values[0])), last + 2);
  const auto verdict = check_linearizable(history, CounterModel{});
  if (!verdict.linearizable) return "linearizability: " + verdict.why;
  if (committed == 0) return "nothing committed";
  return std::nullopt;
}

// An advert set as a node starts, before it has met any peer, reaches a peer it meets after: on
// a heartbeat of 30 s, not at the next exchange but at the first contact (Lir's finding on 0.9.1:
// the push found no peers, and the bootstrap peer kept the handshake's older record).
TEST(SimNodes, AnAdvertSetBeforeAPeerIsMetReachesItAtFirstContact) {
  for (uint64_t seed = 1; seed <= 4; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    World world(seed);
    SimNodes nodes(world, 2, {}, 0, 0, false);
    nodes.configure = [](size_t, NodeConfig& config) {
      config.heartbeat = std::chrono::seconds(30);
      config.dead_after = std::chrono::seconds(90);
    };
    nodes.host = [&](size_t i, Node& node, bool) {
      if (i != 1) return;
      node.context().nodes().advertise("lir", net::Bytes{1}, std::chrono::seconds(1), world.io.get_executor(), [](membership::Pushed) {});
    };
    nodes.start_all();
    const auto held = [&] {
      for (const auto& record : nodes.node(0).context().nodes().roster())
        if (record.id == nodes.id(1) && record.advert("lir")) return true;
      return false;
    };
    const auto until = world.now_ms() + 2000;
    world.run_until([&] { return held() || world.now_ms() >= until; }, 2'000'000);
    EXPECT_TRUE(held());
  }
}

// The fixed seeds, or DANNAN_SIM_SEEDS for a longer run by hand.
uint64_t seeds(uint64_t fallback) {
  const char* set = std::getenv("DANNAN_SIM_SEEDS");
  return set && *set ? std::strtoull(set, nullptr, 10) : fallback;
}

// A seed replays exactly with agreement running over the nodes too: every operation told the
// same at the same time. What a start draws fresh (the durability epoch, which seeds agreement's
// backoff) comes from the seed, not the operating system.
TEST(SimNodes, ASeedReplaysExactlyWithAgreementOverTheNodes) {
  for (uint64_t seed = 1; seed <= 4; ++seed) {
    std::string first, second;
    ASSERT_FALSE(agreement_over_nodes(seed, true, &first)) << "seed " << seed;
    ASSERT_FALSE(agreement_over_nodes(seed, true, &second)) << "seed " << seed;
    EXPECT_EQ(first, second) << "seed " << seed;
  }
}

// Two rosters naming different sets, from real membership (the agreement design's test): split 3 and
// 2, each side's changes name the set its own roster gives as well as the genesis set. Neither a
// roster's set (not the slot's) nor the minority commits; the history is linearizable.
TEST(SimNodes, TwoRostersNamingDifferentSetsNeverBothCommit) {
  for (uint64_t seed = 1; seed <= seeds(10); ++seed) {
    const auto failed = agreement_over_nodes(seed, true);
    ASSERT_FALSE(failed) << "seed " << seed << ": " << *failed;
  }
}

TEST(SimNodes, AgreementOverRealMembershipStaysLinearizableUnderFaults) {
  for (uint64_t seed = 1; seed <= seeds(10); ++seed) {
    const auto failed = agreement_over_nodes(seed);
    ASSERT_FALSE(failed) << "seed " << seed << ": " << *failed;
  }
}

}  // namespace
}  // namespace dannan::test::sim
