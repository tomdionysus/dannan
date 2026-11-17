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

std::optional<std::string> agreement_over_nodes(uint64_t seed) {
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
    auto again = std::make_shared<std::function<void()>>();
    *again = [&, i, store = stores[i], again] {
      if (!nodes.up(i) || stores[i] != store) return;
      nodes.node(i).context().agreement().catch_up(space, 0, {agree::Name{'c'}}, nodes.ids(), 0, 1000ms, world.io.get_executor(),
                                                   [&, i, store, again](agree::Outcome outcome) {
                                                     if (outcome.committed) return store->set_ready(true);
                                                     world.after(100, *again);
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
  std::vector<Counting> ops(30);
  const auto start = world.now_ms();
  for (size_t n = 0; n < ops.size(); ++n)
    world.after(world.random.below(1500), [&, n] {
      size_t at = world.random.below(nodes.size());
      for (size_t tries = 0; tries < nodes.size() && !nodes.up(at); ++tries) at = (at + 1) % nodes.size();
      if (!nodes.up(at)) return;
      ops[n].invoked = world.now_ms();
      nodes.node(at).context().agreement().change(
          space, 0, {agree::Name{'c'}}, nodes.ids(), 0,
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
  // Restarts: node 1 keeping its store, node 3 as itself with its store lost.
  world.after(400, [&] { nodes.restart(1); });
  world.after(900, [&] { nodes.restart(3, Wipe::store); });
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

TEST(SimNodes, AgreementOverRealMembershipStaysLinearizableUnderFaults) {
  for (uint64_t seed = 1; seed <= 10; ++seed) {
    const auto failed = agreement_over_nodes(seed);
    ASSERT_FALSE(failed) << "seed " << seed << ": " << *failed;
  }
}

}  // namespace
}  // namespace dannan::test::sim
