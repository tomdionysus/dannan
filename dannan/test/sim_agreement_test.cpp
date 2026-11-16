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
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "dannan/agree/wire.h"
#include "dannan/net/routes.h"
#include "dannan/test/fixtures/sim/agreement.h"

namespace dannan::test::sim {
namespace {

using namespace std::chrono_literals;

constexpr agree::Slot counter_slot = 0;

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

// The store contract a value-less prepare relies on: a load without values leaves them
// empty and says what is held; keeping a state held but not carried leaves the value and its
// summary alone; a state carrying a value is given the store's summary.
TEST(SimStore, AValueLessLoadKeepsTheValueAndItsSummary) {
  Store store(true);
  const agree::Name k{'k'};
  const agree::Bytes value(1000, 5);
  agree::Held written;
  written.names = {k};
  written.states = {agree::AcceptorState{agree::Ballot{1, {}, {}}, agree::Ballot{1, {}, {}}, value, agree::Ballot{1, {}, {}}, {}, true}};
  store.keep(counter_slot, written, [](bool kept) { ASSERT_TRUE(kept); });
  ASSERT_TRUE(store.state(counter_slot, k));
  EXPECT_EQ(store.state(counter_slot, k)->summary, Store::summary(value));

  std::optional<agree::Held> loaded;
  store.load(counter_slot, std::vector<agree::Name>{k}, false, false, [&](agree::Held held) { loaded = std::move(held); });
  ASSERT_TRUE(loaded);
  ASSERT_EQ(loaded->states.size(), 1u);
  EXPECT_FALSE(loaded->states[0].value);
  EXPECT_TRUE(loaded->states[0].has_value);
  EXPECT_EQ(loaded->states[0].summary, Store::summary(value));

  loaded->states[0].promised = agree::Ballot{2, {}, {}};
  store.keep(counter_slot, *loaded, [](bool kept) { ASSERT_TRUE(kept); });
  const auto after = store.state(counter_slot, k);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->promised, (agree::Ballot{2, {}, {}}));
  ASSERT_TRUE(after->value);
  EXPECT_EQ(*after->value, value);
  EXPECT_EQ(after->summary, Store::summary(value));
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

// One seeded run; `mode` says how a crashed node comes back.
enum class Mode {
  // A crashed node restarts with its durable state.
  keep,
  // It restarts with its state lost and votes at once: the design forbids it.
  broken,
  // It restarts with its state lost and refuses to vote until caught up (the design's rule).
  catch_up,
};

Run simulate(uint64_t seed, Mode mode) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 8;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  Cluster cluster(world, 5, faults);
  std::vector<Op> history(40);
  // How a crashed node comes back, by the mode.
  std::vector<uint64_t> incarnation(cluster.size(), 0);
  std::function<void(size_t, uint64_t)> catch_up = [&](size_t a, uint64_t at) {
    if (!cluster.up(a) || incarnation[a] != at) return;
    cluster.agreement(a).catch_up(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, 1000ms, world.io.get_executor(),
                                  [&, a, at](agree::Outcome outcome) {
                                    if (!cluster.up(a) || incarnation[a] != at) return;
                                    if (outcome.committed) return cluster.store(a).set_ready(true);
                                    world.after(100, [&, a, at] { catch_up(a, at); });
                                  });
  };
  const auto back = [&](size_t a) {
    ++incarnation[a];
    switch (mode) {
      case Mode::keep:
        return cluster.restart(a);
      case Mode::broken:
        // Its state lost, it votes at once: what the design forbids.
        return cluster.restart(a, true, true);
      case Mode::catch_up: {
        // Its state lost, it refuses to vote until it has caught up. The rule assumes a
        // minority loses its state at once: a majority wiped has nothing left to catch up
        // from, and correctly never commits again. So a node is wiped only while those not
        // yet caught up stay a minority; otherwise it comes back with its state.
        size_t behind = 0;
        for (size_t i = 0; i < cluster.size(); ++i)
          if (!cluster.store(i).ready()) ++behind;
        if (behind + 1 > (cluster.size() - 1) / 2) return cluster.restart(a);
        cluster.restart(a, true, false);
        return catch_up(a, incarnation[a]);
      }
    }
  };
  // Clients on every node, at random times over two simulated seconds.
  for (size_t n = 0; n < history.size(); ++n) {
    world.after(world.random.below(2000), [&, n] {
      size_t at = world.random.below(cluster.size());
      for (size_t tries = 0; tries < cluster.size() && !cluster.up(at); ++tries) at = (at + 1) % cluster.size();
      if (!cluster.up(at)) return;
      history[n].invoked = world.now_ms();
      cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, increment(), {}, 3000ms,
                                   world.io.get_executor(), [&, n](agree::Outcome outcome) {
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
          if (!cluster.up(a)) back(a);
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
      if (!cluster.up(i)) back(i);
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
  cluster.agreement(0).read(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, {}, 5000ms, world.io.get_executor(),
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

// the maintainer's ruling (2026-11-15: "No root except at genesis. A root may start with only one
// node."), under the world's faults: founded on one node, every slot is grown to five by
// transfers, one node at a time, while clients increment a counter naming whichever set they
// last heard of. A client refused for a stale set is told the set and goes there, each try
// its own operation (a refused one may still have taken effect). Every slot ends on the five,
// and the history is linearizable.
Run grow(uint64_t seed) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 10;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  Cluster cluster(world, 5, faults, true, 1);
  const auto first = [&](size_t size) {
    std::vector<identity::NodeId> out;
    for (size_t i = 0; i < size; ++i) out.push_back(cluster.id(i));
    return out;
  };
  // What placement says the counter's slot is on, as clients last heard.
  std::vector<identity::NodeId> placed = first(1);
  uint64_t placed_version = 0;
  std::vector<size_t> grown(Cluster::slots, 1);
  bool founded = false;
  cluster.agreement(0).found(Cluster::space, 5000ms, world.io.get_executor(), [&](agree::Outcome outcome) { founded = outcome.committed; });
  // Each slot grows from the founder: one more node per committed transfer.
  std::function<void(agree::Slot)> grow_slot = [&](agree::Slot slot) {
    const auto size = grown[slot];
    if (size == cluster.size()) return;
    cluster.agreement(0).transfer(Cluster::space, slot, first(size), size - 1, first(size + 1), size, 3000ms, world.io.get_executor(),
                                  [&, slot, size](agree::Outcome outcome) {
                                    if (outcome.committed) {
                                      grown[slot] = size + 1;
                                      if (slot == counter_slot) {
                                        placed = first(size + 1);
                                        placed_version = size;
                                      }
                                    }
                                    world.after(50, [&, slot] { grow_slot(slot); });
                                  });
  };
  world.after(10, [&] {
    for (agree::Slot slot = 0; slot < Cluster::slots; ++slot) grow_slot(slot);
  });
  std::deque<Op> history;
  std::function<void(std::vector<identity::NodeId>, uint64_t, int)> increment_on = [&](std::vector<identity::NodeId> nodes, uint64_t version, int tries) {
    // A client at a node of the set it names.
    const auto at = nodes[world.random.below(nodes.size())];
    size_t index = 0;
    while (cluster.id(index) != at) ++index;
    if (!cluster.up(index)) return;
    history.emplace_back();
    const size_t n = history.size() - 1;
    history[n].invoked = world.now_ms();
    cluster.agreement(index).change(Cluster::space, counter_slot, {agree::Name{'c'}}, nodes, version, increment(), {}, 3000ms, world.io.get_executor(),
                                    [&, n, tries](agree::Outcome outcome) {
                                      auto& op = history[n];
                                      op.completed = world.now_ms();
                                      op.answered = true;
                                      op.committed = outcome.committed;
                                      if (outcome.committed) op.result = decode(outcome.result);
                                      if (!outcome.committed && outcome.serving && tries < 5)
                                        increment_on(outcome.serving->nodes, outcome.serving->version, tries + 1);
                                    });
  };
  for (int n = 0; n < 30; ++n)
    world.after(20 + world.random.below(2000), [&] {
      if (founded) increment_on(placed, placed_version, 0);
    });
  // Faults: one-way partitions, and crashes and restarts of any node but the founder.
  for (int f = 0; f < 8; ++f)
    world.after(world.random.below(2000), [&] {
      const auto a = 1 + world.random.below(cluster.size() - 1);
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
          if (!cluster.up(a)) cluster.restart(a);
          break;
      }
    });
  const auto start = world.now_ms();
  world.after(2500, [&] {
    cluster.unblock_all();
    cluster.faults().drop_one_in = 0;
    cluster.faults().duplicate_one_in = 0;
    for (size_t i = 0; i < cluster.size(); ++i)
      if (!cluster.up(i)) cluster.restart(i);
  });
  const auto settled = [&] {
    return world.now_ms() >= start + 2500 && std::all_of(grown.begin(), grown.end(), [&](size_t size) { return size == cluster.size(); }) &&
           std::all_of(history.begin(), history.end(), [](const Op& op) { return op.answered; });
  };
  Run run;
  if (!world.run_until(settled, 2'000'000)) {
    run.unsettled = true;
    run.violation = "the run did not settle";
    return run;
  }
  // Every slot is on the five at a majority of them.
  const auto five = agree::ReplicaSet::of(cluster.ids(), cluster.size() - 1);
  for (agree::Slot slot = 0; slot < Cluster::slots; ++slot) {
    size_t holding = 0;
    for (size_t i = 0; i < cluster.size(); ++i) {
      const auto record = cluster.store(i).slot(slot);
      if (record && record->served == five) ++holding;
    }
    if (holding < five.majority()) {
      run.violation = "slot " + std::to_string(slot) + " is on the five at only " + std::to_string(holding) + " nodes";
      return run;
    }
  }
  std::optional<agree::Outcome> read;
  cluster.agreement(4).read(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), five.version, {}, 5000ms, world.io.get_executor(),
                            [&](agree::Outcome outcome) { read = std::move(outcome); });
  world.run_until([&] { return read.has_value(); }, 2'000'000);
  if (!read || !read->committed) {
    run.unsettled = true;
    run.violation = "the final read did not commit" + (read ? ": " + read->why : std::string());
    return run;
  }
  const std::vector<Op> invoked(history.begin(), history.end());
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
    const auto run = simulate(seed, Mode::keep);
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
    const auto run = simulate(seed, Mode::broken);
    // A refusal by the checker, not a run that merely failed to finish.
    if (run.violation && !run.unsettled) {
      caught = run.violation;
      at = seed;
    }
  }
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checker is unproven";
  SUCCEED() << "seed " << at << ": " << *caught;
}

// The design's restart rule: a node that lost its state refuses to vote, catches up by a
// strict read at a majority of the others, and only then votes. Linearizable throughout.
TEST(Simulation, ANodeThatLostItsStateCatchesUpBeforeItVotes) {
  for (uint64_t seed = 1; seed <= seeds(100); ++seed) {
    const auto run = simulate(seed, Mode::catch_up);
    ASSERT_FALSE(run.violation) << "seed " << seed << ": " << *run.violation;
  }
}

TEST(Simulation, AOneNodeGenesisGrowsToFiveUnderFaults) {
  size_t committed = 0;
  for (uint64_t seed = 1; seed <= seeds(60); ++seed) {
    const auto run = grow(seed);
    ASSERT_FALSE(run.violation) << "seed " << seed << ": " << *run.violation;
    committed += run.committed;
  }
  EXPECT_GT(committed, 0u);
}

// The slot table is the cluster's, the same on every node: a node configured with another
// count is refused by every acceptor, so it neither commits nor counts towards a majority,
// while the rest carry on.
TEST(Simulation, ANodeWithAnotherSlotTableIsRefused) {
  World world(7);
  Cluster cluster(world, 3, NetworkFaults{});
  cluster.restart_with_slots(2, Cluster::slots * 2);
  std::optional<agree::Outcome> odd;
  cluster.agreement(2).change(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, increment(), {}, 500ms,
                              world.io.get_executor(), [&](agree::Outcome outcome) { odd = std::move(outcome); });
  ASSERT_TRUE(world.run_until([&] { return odd.has_value(); }, 2'000'000));
  EXPECT_FALSE(odd->committed);
  // The two that agree on the table still commit, without the odd one.
  std::optional<agree::Outcome> even;
  cluster.agreement(0).change(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, increment(), {}, 2000ms,
                              world.io.get_executor(), [&](agree::Outcome outcome) { even = std::move(outcome); });
  ASSERT_TRUE(world.run_until([&] { return even.has_value(); }, 2'000'000));
  ASSERT_TRUE(even->committed) << even->why;
  EXPECT_EQ(std::count(even->accepted.begin(), even->accepted.end(), cluster.id(2)), 0);
}

// Finding a moved slot (Kvasir's review): each node's advert says which slots its records
// serve, so a node never told where a slot went finds it there. The advert carries no
// version, so a change on the hint with a guessed one is refused, told the set and its
// version, and commits there.
TEST(Simulation, AMovedSlotIsFoundFromTheAdvertsAndTheRefusal) {
  World world(11);
  Cluster cluster(world, 5, NetworkFaults{}, true, 3);
  const auto told = [&](auto start) {
    std::optional<agree::Outcome> out;
    start([&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 2'000'000);
    return out.value_or(agree::Outcome{});
  };
  const auto founded = told([&](auto done) { cluster.agreement(0).found(Cluster::space, 2000ms, world.io.get_executor(), done); });
  ASSERT_TRUE(founded.committed) << founded.why;
  EXPECT_EQ(cluster.agreement(4).hint(Cluster::space, 1), cluster.ids({0, 1, 2}));
  const auto moved = told([&](auto done) {
    cluster.agreement(2).transfer(Cluster::space, 1, cluster.ids({0, 1, 2}), 0, cluster.ids({2, 3, 4}), 1, 2000ms, world.io.get_executor(), done);
  });
  ASSERT_TRUE(moved.committed) << moved.why;
  EXPECT_EQ(cluster.agreement(0).hint(Cluster::space, 1), cluster.ids({2, 3, 4}));
  EXPECT_EQ(cluster.agreement(0).hint(Cluster::space, 0), cluster.ids({0, 1, 2}));
  const auto guessed = told([&](auto done) {
    cluster.agreement(3).change(Cluster::space, 1, {agree::Name{'m'}}, cluster.agreement(3).hint(Cluster::space, 1), 0, increment(), {}, 2000ms,
                                world.io.get_executor(), done);
  });
  EXPECT_FALSE(guessed.committed);
  ASSERT_TRUE(guessed.serving);
  EXPECT_EQ(*guessed.serving, agree::ReplicaSet::of(cluster.ids({2, 3, 4}), 1));
  const auto redirected = told([&](auto done) {
    cluster.agreement(3).change(Cluster::space, 1, {agree::Name{'m'}}, guessed.serving->nodes, guessed.serving->version, increment(), {}, 2000ms,
                                world.io.get_executor(), done);
  });
  EXPECT_TRUE(redirected.committed) << redirected.why;
}

// A change that appends one byte, its result the new length.
agree::Change append(uint8_t byte) {
  return [byte](std::span<const std::optional<agree::Bytes>> current) {
    auto next = current[0].value_or(agree::Bytes{});
    next.push_back(byte);
    return agree::Changed{{next}, encode(next.size())};
  };
}

agree::Change put(agree::Bytes value) {
  return [value](std::span<const std::optional<agree::Bytes>> current) { return agree::Changed{{value}, encode(current.size())}; };
}

// Whether any prepare answer that crossed the network carried a value.
bool a_prepare_answer_carried_a_value(const Cluster& cluster) {
  for (const auto& crossed : cluster.crossed) {
    if (crossed.type != net::message::agree_prepare) continue;
    const auto answer = agree::decode_prepare_answer(crossed.answer);
    for (const auto& state : answer.states)
      if (state.value) return true;
  }
  return false;
}

size_t crossed_of(const Cluster& cluster, net::MessageType type) {
  return static_cast<size_t>(std::count_if(cluster.crossed.begin(), cluster.crossed.end(), [type](const Cluster::Crossed& each) { return each.type == type; }));
}

// Value-less prepare through the runtime (the design's "Value-less prepare and deltas"): a
// megabyte register is changed by another node with no prepare answer carrying its value,
// and the change works from the value as it was.
TEST(Simulation, AMegabyteRegisterChangesWithNoPrepareAnswerCarryingItsValue) {
  World world(17);
  Cluster cluster(world, 3, NetworkFaults{});
  cluster.recording = true;
  const auto change_at = [&](size_t at, agree::Change change) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change), {}, 5000ms,
                                 world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 5'000'000);
    return out.value_or(agree::Outcome{});
  };
  const agree::Bytes megabyte(1024 * 1024, 'a');
  const auto written = change_at(0, put(megabyte));
  ASSERT_TRUE(written.committed) << written.why;
  const auto appended = change_at(1, append('b'));
  ASSERT_TRUE(appended.committed) << appended.why;
  EXPECT_EQ(decode(appended.result), megabyte.size() + 1);
  ASSERT_TRUE(appended.values[0]);
  EXPECT_EQ(appended.values[0]->size(), megabyte.size() + 1);
  EXPECT_EQ(appended.values[0]->back(), 'b');
  EXPECT_FALSE(a_prepare_answer_carried_a_value(cluster));
}

// This node's acceptor missed the last write: the base is fetched from another holder, over
// the network and in pieces when larger than one, and the change works from it.
TEST(Simulation, ABaseIsFetchedFromAnotherHolderInPiecesWhenThisNodesAcceptorIsBehind) {
  World world(19);
  Cluster cluster(world, 3, NetworkFaults{});
  const auto change_at = [&](size_t at, agree::Change change) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change), {}, 5000ms,
                                 world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 5'000'000);
    return out.value_or(agree::Outcome{});
  };
  // Three and a half pieces, written by node 0 while node 2 hears nothing from it.
  const agree::Bytes large(3 * agree::max_piece + agree::max_piece / 2, 'x');
  cluster.block(0, 2);
  ASSERT_TRUE(change_at(0, put(large)).committed);
  ASSERT_FALSE(cluster.store(2).state(counter_slot, agree::Name{'m'}));
  cluster.unblock_all();
  cluster.recording = true;
  const auto appended = change_at(2, append('y'));
  ASSERT_TRUE(appended.committed) << appended.why;
  EXPECT_EQ(decode(appended.result), large.size() + 1);
  EXPECT_FALSE(a_prepare_answer_carried_a_value(cluster));
  // Four pieces from one holder, or more if a holder gave way to another.
  EXPECT_GE(crossed_of(cluster, net::message::agree_fetch), 4u);
}

// The owner's reduction through the runtime: it asks for an answer behind the base, which is
// fetched by that answer's own ballot (here from this node's own acceptor), and it sees that
// older value.
TEST(Simulation, AnAnswerTheReductionAsksForIsFetchedByItsOwnBallot) {
  World world(23);
  Cluster cluster(world, 3, NetworkFaults{});
  const auto change_at = [&](size_t at, agree::Change change, agree::Reduce reduce) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change),
                                 std::move(reduce), 5000ms, world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 5'000'000);
    return out.value_or(agree::Outcome{});
  };
  ASSERT_TRUE(change_at(0, put(agree::Bytes{'o', 'l', 'd'}), {}).committed);
  // Node 2 misses the next write.
  cluster.block(0, 2);
  ASSERT_TRUE(change_at(0, put(agree::Bytes{'n', 'e', 'w'}), {}).committed);
  cluster.unblock_all();
  std::vector<std::string> seen;
  const agree::Reduce older = [&seen](const agree::NameState& base, std::span<const agree::NameState> answers) {
    agree::Reduced out{base.value, {}};
    for (size_t i = 0; i < answers.size(); ++i) {
      if (answers[i].accepted == base.accepted) continue;
      if (!answers[i].value) {
        out.need.push_back({i, std::nullopt});
        continue;
      }
      seen.emplace_back(answers[i].value->begin(), answers[i].value->end());
    }
    return out;
  };
  const auto read = change_at(2, append('!'), older);
  ASSERT_TRUE(read.committed) << read.why;
  ASSERT_TRUE(read.values[0]);
  EXPECT_EQ(std::string(read.values[0]->begin(), read.values[0]->end()), "new!");
  EXPECT_EQ(seen, std::vector<std::string>{"old"});
}

// The fast path through the runtime: a node's second change of the same names commits in
// one round; another node's change in between sends its next one the full way, still
// committed, and the counter counts every increment once.
TEST(Simulation, TheFastPathIsTakenUncontendedAndLeftWhenContended) {
  World world(5);
  Cluster cluster(world, 3, NetworkFaults{});
  const auto increment_at = [&](size_t at) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, increment(), {}, 2000ms,
                                 world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 2'000'000);
    return out.value_or(agree::Outcome{});
  };
  const auto first = increment_at(0);
  ASSERT_TRUE(first.committed) << first.why;
  EXPECT_FALSE(first.fast);
  const auto second = increment_at(0);
  ASSERT_TRUE(second.committed) << second.why;
  EXPECT_TRUE(second.fast);
  EXPECT_EQ(decode(second.result), 2u);
  ASSERT_TRUE(increment_at(1).committed);
  const auto contended = increment_at(0);
  ASSERT_TRUE(contended.committed) << contended.why;
  EXPECT_FALSE(contended.fast);
  EXPECT_EQ(decode(contended.result), 4u);
  const auto after = increment_at(0);
  EXPECT_TRUE(after.fast);
  EXPECT_EQ(decode(after.result), 5u);
}

// A seed replays exactly.
TEST(Simulation, ASeedReplaysExactly) {
  const auto first = simulate(42, Mode::keep);
  const auto again = simulate(42, Mode::keep);
  EXPECT_EQ(first.committed, again.committed);
  EXPECT_EQ(first.violation, again.violation);
}

}  // namespace
}  // namespace dannan::test::sim
