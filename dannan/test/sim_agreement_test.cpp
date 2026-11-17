//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Agreement under the simulation harness (TODO/2026-11-15-simulation-harness.md): five
// nodes running Dannan's real Agreement increment one counter from every side while the
// world loses, delays, duplicates and one-way-blocks messages and crashes and restarts
// nodes. Agreement's own invariants are checked on every step and the history for
// linearizability; and the checkers must refuse each protocol broken on purpose, naming the
// seed, or they have never been shown to refuse anything (Miach).

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dannan/agree/wire.h"
#include "dannan/net/routes.h"
#include "dannan/test/fixtures/sim/agreement.h"
#include "dannan/test/fixtures/sim/durability.h"
#include "dannan/test/fixtures/sim/history.h"
#include "dannan/test/fixtures/sim/invariants.h"
#include "dannan/test/fixtures/sim/linearizable.h"
#include "dannan/test/fixtures/sim/models.h"

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

// The counter's order for invariant 2: increments and strict reads only ever leave it where it
// was or higher.
Invariants::Options counter_order() {
  return {[](const std::optional<agree::Bytes>& earlier, const std::optional<agree::Bytes>& later) { return decode(later) >= decode(earlier); }};
}

// The broken reduction: the oldest answer's value rather than the base's, rolling the base back
// (the Reduce contract forbids it).
agree::Reduce rolling_back() {
  return [](const agree::NameState& base, std::span<const agree::NameState> answers) {
    size_t oldest = 0;
    for (size_t i = 1; i < answers.size(); ++i)
      if (answers[i].accepted < answers[oldest].accepted) oldest = i;
    if (answers.empty() || answers[oldest].accepted == base.accepted) return agree::Reduced{base.value, {}};
    if (!answers[oldest].value && answers[oldest].has_value) return agree::Reduced{std::nullopt, {{oldest, std::nullopt}}};
    return agree::Reduced{answers[oldest].value, {}};
  };
}

// The checker of agreement's own invariants, unit by unit, on states put straight into the
// stores: it must refuse each breach it names and pass a restart it is told of.
void hold(Store& store, agree::Ballot promised, agree::Ballot accepted, std::optional<agree::Bytes> value, std::optional<agree::ReplicaSet> served = {}) {
  agree::Held held;
  held.slot.served = std::move(served);
  held.names = {agree::Name{'k'}};
  agree::AcceptorState state;
  state.promised = promised;
  state.accepted = accepted;
  state.value = std::move(value);
  state.changed_at = accepted;
  held.states = {state};
  store.keep(counter_slot, held, [](Store::Kept kept) { ASSERT_TRUE(kept.kept); });
}

agree::Ballot ballot(uint64_t counter) { return agree::Ballot{counter, {}, {}}; }

TEST(Invariants, RefuseAPromiseThatFalls) {
  World world(1);
  Cluster cluster(world, 3, NetworkFaults{});
  Invariants invariants(world, cluster, {});
  hold(cluster.store(0), ballot(5), {}, std::nullopt);
  invariants.check();
  ASSERT_FALSE(invariants.violation());
  hold(cluster.store(0), ballot(3), {}, std::nullopt);
  invariants.check();
  ASSERT_TRUE(invariants.violation());
  EXPECT_NE(invariants.violation()->find("below the promise 5"), std::string::npos) << *invariants.violation();
}

TEST(Invariants, RefuseAnAcceptBelowAPromiseHeld) {
  World world(1);
  Cluster cluster(world, 3, NetworkFaults{});
  Invariants invariants(world, cluster, {});
  hold(cluster.store(1), ballot(5), {}, std::nullopt);
  invariants.check();
  hold(cluster.store(1), ballot(5), ballot(4), encode(1));
  invariants.check();
  ASSERT_TRUE(invariants.violation());
  EXPECT_NE(invariants.violation()->find("accepted 4/0, below the promise 5"), std::string::npos) << *invariants.violation();
}

TEST(Invariants, RefuseTwoValuesAtOneBallot) {
  World world(1);
  Cluster cluster(world, 3, NetworkFaults{});
  Invariants invariants(world, cluster, {});
  hold(cluster.store(0), ballot(2), ballot(2), encode(1));
  hold(cluster.store(1), ballot(2), ballot(2), encode(2));
  invariants.check();
  ASSERT_TRUE(invariants.violation());
  EXPECT_NE(invariants.violation()->find("accepted differently at the same ballot"), std::string::npos) << *invariants.violation();
}

TEST(Invariants, RefuseAValueThatDoesNotAdvanceTheOneChosen) {
  World world(1);
  Cluster cluster(world, 3, NetworkFaults{});
  Invariants invariants(world, cluster, counter_order());
  const auto set = agree::ReplicaSet::of(cluster.ids(), 0);
  hold(cluster.store(0), ballot(2), ballot(2), encode(5), set);
  hold(cluster.store(1), ballot(2), ballot(2), encode(5), set);
  invariants.check();
  ASSERT_FALSE(invariants.violation());
  EXPECT_EQ(invariants.chosen(), 1u);
  // A higher ballot that advances it is fine; one that rolls it back is not.
  hold(cluster.store(2), ballot(3), ballot(3), encode(6), set);
  invariants.check();
  ASSERT_FALSE(invariants.violation());
  hold(cluster.store(2), ballot(4), ballot(4), encode(4), set);
  invariants.check();
  ASSERT_TRUE(invariants.violation());
  EXPECT_NE(invariants.violation()->find("does not advance the value chosen at 2/0"), std::string::npos) << *invariants.violation();
}

// The same breach found the other way round: the higher value was seen first, and the lower
// ballot is chosen after it.
TEST(Invariants, RefuseAChosenValueThatAnEarlierHigherAcceptDoesNotAdvance) {
  World world(1);
  Cluster cluster(world, 3, NetworkFaults{});
  Invariants invariants(world, cluster, counter_order());
  const auto set = agree::ReplicaSet::of(cluster.ids(), 0);
  hold(cluster.store(2), ballot(4), ballot(4), encode(1), set);
  hold(cluster.store(0), ballot(2), ballot(2), encode(5), set);
  invariants.check();
  ASSERT_FALSE(invariants.violation());
  hold(cluster.store(1), ballot(2), ballot(2), encode(5), set);
  invariants.check();
  ASSERT_TRUE(invariants.violation());
  EXPECT_NE(invariants.violation()->find("the value accepted at 4/0 does not advance"), std::string::npos) << *invariants.violation();
}

// A restart without state, told of, starts the acceptor afresh; the same loss untold is caught.
TEST(Invariants, PassARestartWithoutStateTheyAreToldOfAndCatchOneUntold) {
  World world(1);
  Cluster cluster(world, 3, NetworkFaults{});
  Invariants invariants(world, cluster, {});
  hold(cluster.store(0), ballot(5), {}, std::nullopt);
  invariants.check();
  cluster.crash(0);
  cluster.restart(0, true);
  hold(cluster.store(0), ballot(3), {}, std::nullopt);
  invariants.check();
  EXPECT_FALSE(invariants.violation());

  cluster.store(1).lying_sync = true;
  hold(cluster.store(1), ballot(5), {}, std::nullopt);
  invariants.check();
  cluster.crash(1);
  cluster.restart(1);
  invariants.check();
  ASSERT_TRUE(invariants.violation());
  EXPECT_NE(invariants.violation()->find("forgot the promise 5"), std::string::npos) << *invariants.violation();
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
  store.keep(counter_slot, written, [](Store::Kept kept) { ASSERT_TRUE(kept.kept); });
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
  store.keep(counter_slot, *loaded, [](Store::Kept kept) { ASSERT_TRUE(kept.kept); });
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

// The counter's history, checked by the harness's linearizability checker against its counter
// model: each increment an operation (one not committed may or may not have taken effect),
// then a read of the final value after everything.
// The counter's own check, exact and linear after sorting: increments only, each committed one's
// result the counter's value just after it, so that result is its place in any order. Every
// result must be distinct and at most the final value; then, place by place from 1 to the
// final value, each increment takes the earliest instant it can (after the previous one and its
// invocation), a committed one by its result and not after its answer, a gap by the unanswered
// increment invoked earliest. Every increment is +1, so taking the earliest is never worse.
std::optional<std::string> check_counter(const std::vector<Op>& ops, uint64_t final_value) {
  std::map<uint64_t, const Op*> placed;
  std::vector<uint64_t> unanswered;
  for (const auto& op : ops) {
    if (!op.committed) {
      unanswered.push_back(op.invoked);
      continue;
    }
    if (op.result == 0 || op.result > final_value) return "a committed increment's result " + std::to_string(op.result) + " is outside 1 to the final value";
    if (!placed.emplace(op.result, &op).second) return "two committed increments both say " + std::to_string(op.result);
  }
  if (final_value - placed.size() > unanswered.size()) return "the final value counts more increments than were made";
  std::sort(unanswered.begin(), unanswered.end());
  size_t next_unanswered = 0;
  uint64_t at = 0;
  for (uint64_t place = 1; place <= final_value; ++place) {
    const auto committed = placed.find(place);
    if (committed != placed.end()) {
      at = std::max(at, committed->second->invoked);
      if (at > committed->second->completed) return "no order puts the increment that answered " + std::to_string(place) + " in its place in time";
      continue;
    }
    if (next_unanswered == unanswered.size()) return "nothing fills place " + std::to_string(place);
    at = std::max(at, unanswered[next_unanswered++]);
  }
  return std::nullopt;
}

std::optional<std::string> check(const std::vector<Op>& ops, uint64_t final_value) {
  // The counter's exact check decides; the general search (Wing-Gong) cross-checks it wherever
  // it decides within its usual budget, so a disagreement between them is found too.
  const auto counted = check_counter(ops, final_value);
  History history;
  uint64_t last = 0;
  for (size_t i = 0; i < ops.size(); ++i) {
    const auto at = history.invoke(i, {"c"}, Bytes{'i'}, ops[i].invoked);
    if (ops[i].committed) history.answer(at, CounterModel::number(ops[i].result), ops[i].completed);
    last = std::max({last, ops[i].invoked, ops[i].completed});
  }
  const auto read = history.invoke(ops.size(), {"c"}, Bytes{'r'}, last + 1);
  history.answer(read, CounterModel::number(final_value), last + 2);
  // Late commits overlap, so the general search can need tens of millions of steps (94 million
  // at one fixed seed, 2026-11-17): it runs within its usual budget, and where it runs out the
  // counter's check stands alone.
  const auto verdict = check_linearizable(history, CounterModel{}, 250'000);
  if (!verdict.exhausted && verdict.linearizable == counted.has_value())
    return "the two checkers disagree: the counter's says " + counted.value_or("linearizable") + ", the search " +
           (verdict.linearizable ? std::string("linearizable") : verdict.why);
  return counted;
}

// The counter's check, on histories small enough to see: it passes what is right and names
// each way of being wrong.
TEST(CounterCheck, PassesAndRefusesAsItShould) {
  const auto op = [](uint64_t invoked, uint64_t completed, std::optional<uint64_t> result) {
    return Op{invoked, completed, result.has_value(), result.has_value(), result.value_or(0)};
  };
  EXPECT_FALSE(check_counter({op(1, 5, 1), op(2, 6, 2)}, 2));
  // An unanswered one fills a gap, or never happened.
  EXPECT_FALSE(check_counter({op(1, 5, 2), op(0, 0, std::nullopt)}, 2));
  EXPECT_FALSE(check_counter({op(1, 5, 1), op(0, 0, std::nullopt)}, 1));
  EXPECT_TRUE(check_counter({op(1, 5, 1), op(2, 6, 1)}, 2));
  EXPECT_TRUE(check_counter({op(1, 5, 1)}, 2));
  // Answered 2 before the one that answered 1 began.
  EXPECT_TRUE(check_counter({op(10, 12, 1), op(1, 5, 2)}, 2));
  // The gap at 1 is filled only by an increment invoked after the one at 2 answered.
  EXPECT_TRUE(check_counter({op(1, 5, 2), Op{9, 0, false, false, 0}}, 2));
}

struct Run {
  std::optional<std::string> violation;
  // The run itself did not finish: the harness's failure, not a history the checker refused.
  bool unsettled = false;
  size_t committed = 0;
};

// A run ends: a breach of agreement's invariants, if any, is the refusal, whether or not the
// run went on to settle.
Run finished(Run run, const Invariants& invariants) {
  if (!invariants.violation()) return run;
  run.violation = "invariant: " + *invariants.violation();
  run.unsettled = false;
  return run;
}

// One seeded run; `mode` says how a crashed node comes back, or which part of the protocol is
// broken on purpose.
enum class Mode {
  // A crashed node restarts with its durable state.
  keep,
  // It restarts with its state lost and votes at once: the design forbids it.
  broken,
  // It restarts with its state lost and refuses to vote until caught up (the design's rule).
  catch_up,
  // Broken, each with nodes restarting as in `keep`: every store's sync lies, and the node
  // trusts it; every acceptor loads its promises as no more than it accepted; every client's
  // reduction rolls the base back; a proposer skips prepare, sending an accept built on its own
  // acceptor's value; a proposer whose roster names nodes 2, 3 and 4 changes the counter on
  // them without the proof that they serve its slot.
  lying_sync,
  forget_promises,
  rollback,
  skip_prepare,
  other_roster,
};

Run simulate(uint64_t seed, Mode mode) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 8;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  Cluster cluster(world, 5, faults);
  Invariants invariants(world, cluster, counter_order());
  for (size_t i = 0; i < cluster.size(); ++i) {
    cluster.store(i).lying_sync = mode == Mode::lying_sync;
    cluster.store(i).forget_promises = mode == Mode::forget_promises;
  }
  const auto table = agree::table_fingerprint(Cluster::slots, cluster.genesis());
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
      case Mode::lying_sync:
      case Mode::forget_promises:
      case Mode::rollback:
      case Mode::skip_prepare:
      case Mode::other_roster:
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
      cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, increment(),
                                   mode == Mode::rollback ? rolling_back() : agree::Reduce{}, 3000ms, world.io.get_executor(), [&, n](agree::Outcome outcome) {
                                     auto& op = history[n];
                                     op.completed = world.now_ms();
                                     op.answered = true;
                                     op.committed = outcome.committed;
                                     if (outcome.committed) op.result = decode(outcome.result);
                                   });
    });
  }
  // The broken proposers, built here on the real wire to the real acceptors, each at a ballot
  // above anything the clients reach.
  const auto skip_prepare = [&](uint64_t k) {
    const auto r = world.random.below(cluster.size());
    if (!cluster.up(r)) return;
    // What it takes as current: its own acceptor's value, however far behind.
    const auto held = cluster.store(r).state(counter_slot, agree::Name{'c'});
    agree::Accept accept;
    accept.set = cluster.genesis();
    accept.slot = counter_slot;
    accept.ballot = agree::Ballot{1'000'000 + k, cluster.id(r), {}};
    accept.names = {agree::Name{'c'}};
    accept.values = {encode(decode(held ? held->value : std::nullopt) + 1)};
    accept.changed_at = {accept.ballot};
    const auto payload = agree::encode(agree::AcceptMessage{Cluster::space, accept, table});
    for (size_t to = 0; to < cluster.size(); ++to) cluster.call(r, to, net::message::agree_accept, payload, 500ms, [](auto, auto) {});
  };
  const auto other_roster = [&](uint64_t k) {
    // Its roster puts the slot on nodes 2, 3 and 4, at a version above the genesis set's: the
    // acceptors there promise, as they do for a newer set (the laggard rule), and only the
    // proposer's proof rule would stop it. It has none.
    const auto set = agree::ReplicaSet::of(cluster.ids({2, 3, 4}), Cluster::genesis_version + 1);
    const size_t r = 2 + world.random.below(3);
    if (!cluster.up(r)) return;
    agree::Prepare prepare;
    prepare.set = set;
    prepare.slot = counter_slot;
    prepare.ballot = agree::Ballot{1'000'000 + k, cluster.id(r), {}};
    prepare.names = {agree::Name{'c'}};
    const auto payload = agree::encode(agree::PrepareMessage{Cluster::space, prepare, table});
    auto promised = std::make_shared<std::vector<agree::NameState>>();
    for (const size_t to : {2, 3, 4})
      cluster.call(r, to, net::message::agree_prepare, payload, 500ms, [&, promised, set, prepare, r](std::optional<net::Bytes> reply, std::string) {
        if (!reply || promised->size() >= set.majority()) return;
        const auto answer = agree::decode_prepare_answer(*reply);
        if (answer.refused || answer.states.size() != 1) return;
        promised->push_back(answer.states[0]);
        if (promised->size() < set.majority()) return;
        // The highest of its own majority's answers, changed.
        const auto base = std::max_element(promised->begin(), promised->end(), [](const auto& a, const auto& b) { return a.accepted < b.accepted; });
        agree::Accept accept;
        accept.set = set;
        accept.slot = counter_slot;
        accept.ballot = prepare.ballot;
        accept.names = prepare.names;
        accept.values = {encode(decode(base->value) + 1)};
        accept.changed_at = {prepare.ballot};
        const auto bytes = agree::encode(agree::AcceptMessage{Cluster::space, accept, table});
        for (const size_t at : {2, 3, 4}) cluster.call(r, at, net::message::agree_accept, bytes, 500ms, [](auto, auto) {});
      });
  };
  if (mode == Mode::skip_prepare || mode == Mode::other_roster)
    for (uint64_t k = 0; k < 4; ++k) world.after(world.random.below(2000), [&, k] { mode == Mode::skip_prepare ? skip_prepare(k) : other_roster(k); });
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
    return finished(run, invariants);
  }
  // The final value, by a strict read once healed.
  std::optional<agree::Outcome> read;
  cluster.agreement(0).read(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, {}, 5000ms, world.io.get_executor(),
                            [&](agree::Outcome outcome) { read = std::move(outcome); });
  world.run_until([&] { return read.has_value(); }, 2'000'000);
  if (!read || !read->committed) {
    run.unsettled = true;
    run.violation = "the final read did not commit";
    return finished(run, invariants);
  }
  std::vector<Op> invoked;
  for (const auto& op : history)
    if (op.invoked) invoked.push_back(op);
  run.committed = static_cast<size_t>(std::count_if(invoked.begin(), invoked.end(), [](const Op& op) { return op.committed; }));
  if (const auto refused = check(invoked, decode(read->values[0]))) run.violation = "linearizability: " + *refused;
  return finished(run, invariants);
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
  Invariants invariants(world, cluster, counter_order());
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
    return finished(run, invariants);
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
      return finished(run, invariants);
    }
  }
  std::optional<agree::Outcome> read;
  cluster.agreement(4).read(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), five.version, {}, 5000ms, world.io.get_executor(),
                            [&](agree::Outcome outcome) { read = std::move(outcome); });
  world.run_until([&] { return read.has_value(); }, 2'000'000);
  if (!read || !read->committed) {
    run.unsettled = true;
    run.violation = "the final read did not commit" + (read ? ": " + read->why : std::string());
    return finished(run, invariants);
  }
  const std::vector<Op> invoked(history.begin(), history.end());
  run.committed = static_cast<size_t>(std::count_if(invoked.begin(), invoked.end(), [](const Op& op) { return op.committed; }));
  if (const auto refused = check(invoked, decode(read->values[0]))) run.violation = "linearizability: " + *refused;
  return finished(run, invariants);
}

// The merge class's correct reduction (Mimir's ruling 16): every promiser's merged state, each
// value it lacks asked for, the union of them all.
agree::Reduce uniting() {
  return [](const agree::NameState& base, std::span<const agree::NameState> answers) {
    agree::Reduced out;
    auto merged = base.value.value_or(agree::Bytes{});
    for (size_t i = 0; i < answers.size(); ++i) {
      if (answers[i].value)
        merged = Store::united(std::move(merged), *answers[i].value);
      else if (answers[i].has_value)
        out.need.push_back({i, std::nullopt});
    }
    if (out.need.empty()) out.value = std::move(merged);
    return out;
  };
}

// Mimir's merge class under faults. The register is a set of bytes: merge writes add to it
// outside agreement, each synced on a majority of the nodes up and only then acknowledged at
// `replicated`, while agreement commands add their own and return the set they saw. Ruling 16:
// every command sees every merge write acknowledged before it began. `dropping`: commands use
// the default reduction, the base's value alone, the broken protocol that drops them.
Run merge_class(uint64_t seed, bool dropping) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 10;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  // The register is made on a clean network; the faults start with the workload.
  Cluster cluster(world, 5, NetworkFaults{});
  for (size_t i = 0; i < cluster.size(); ++i) cluster.store(i).merging = true;
  Invariants invariants(world, cluster,
                        {[](const std::optional<agree::Bytes>& earlier, const std::optional<agree::Bytes>& later) {
                           if (!earlier) return true;
                           return later && std::includes(later->begin(), later->end(), earlier->begin(), earlier->end());
                         },
                         true});
  const agree::Name name{'m'};
  Run run;
  // The register made by agreement first: the empty set.
  std::optional<agree::Outcome> made;
  cluster.agreement(0).change(
      Cluster::space, counter_slot, {name}, cluster.ids(), Cluster::genesis_version,
      [](std::span<const std::optional<agree::Bytes>>) { return agree::Changed{{agree::Bytes{}}, {}}; }, {}, 5000ms, world.io.get_executor(),
      [&](agree::Outcome outcome) { made = std::move(outcome); });
  world.run_until([&] { return made.has_value(); }, 2'000'000);
  if (!made || !made->committed) {
    run.unsettled = true;
    run.violation = "the register was not made" + (made ? ": " + made->why : std::string());
    return finished(run, invariants);
  }
  cluster.faults() = faults;
  const auto start = world.now_ms();
  struct Command {
    uint64_t invoked = 0;
    bool answered = false;
    bool committed = false;
    agree::Bytes seen;
  };
  std::vector<Command> commands(20);
  for (size_t n = 0; n < commands.size(); ++n)
    world.after(world.random.below(2000), [&, n] {
      size_t at = world.random.below(cluster.size());
      for (size_t tries = 0; tries < cluster.size() && !cluster.up(at); ++tries) at = (at + 1) % cluster.size();
      if (!cluster.up(at)) return;
      commands[n].invoked = world.now_ms();
      const auto element = static_cast<uint8_t>('a' + n);
      cluster.agreement(at).change(
          Cluster::space, counter_slot, {name}, cluster.ids(), Cluster::genesis_version,
          [element](std::span<const std::optional<agree::Bytes>> current) {
            auto seen = current[0].value_or(agree::Bytes{});
            return agree::Changed{{Store::united(seen, agree::Bytes{element})}, seen};
          },
          dropping ? agree::Reduce{} : uniting(), 3000ms, world.io.get_executor(),
          [&, n](agree::Outcome outcome) {
            commands[n].answered = true;
            commands[n].committed = outcome.committed;
            if (outcome.committed) commands[n].seen = outcome.result;
          });
    });
  std::vector<std::pair<uint8_t, uint64_t>> acknowledged;
  for (uint8_t k = 0; k < 16; ++k)
    world.after(world.random.below(2000), [&, k] {
      std::vector<size_t> up;
      for (size_t i = 0; i < cluster.size(); ++i)
        if (cluster.up(i)) up.push_back(i);
      for (size_t i = up.size(); i > 1; --i) std::swap(up[i - 1], up[world.random.below(i)]);
      const auto majority = cluster.size() / 2 + 1;
      const auto element = static_cast<uint8_t>('A' + k);
      for (size_t i = 0; i < up.size() && i < majority; ++i) cluster.store(up[i]).merge(counter_slot, name, element);
      if (up.size() >= majority) acknowledged.emplace_back(element, world.now_ms());
    });
  for (int f = 0; f < 8; ++f)
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
          if (!cluster.up(a)) cluster.restart(a);
          break;
      }
    });
  world.after(2500, [&] {
    cluster.unblock_all();
    cluster.faults().drop_one_in = 0;
    cluster.faults().duplicate_one_in = 0;
    for (size_t i = 0; i < cluster.size(); ++i)
      if (!cluster.up(i)) cluster.restart(i);
  });
  const auto settled = [&] {
    return world.now_ms() >= start + 2500 &&
           std::all_of(commands.begin(), commands.end(), [](const Command& command) { return command.answered || command.invoked == 0; });
  };
  if (!world.run_until(settled, 2'000'000)) {
    run.unsettled = true;
    run.violation = "the run did not settle";
    return finished(run, invariants);
  }
  for (size_t n = 0; n < commands.size(); ++n) {
    const auto& command = commands[n];
    if (!command.committed) continue;
    ++run.committed;
    for (const auto& [element, at] : acknowledged)
      if (at < command.invoked && !std::binary_search(command.seen.begin(), command.seen.end(), element)) {
        run.violation = "merge: command " + std::to_string(n) + ", begun at " + std::to_string(command.invoked) + " ms, did not see the merge write '" +
                        static_cast<char>(element) + "' acknowledged at " + std::to_string(at) + " ms";
        return finished(run, invariants);
      }
  }
  return finished(run, invariants);
}

// Durability per acknowledgement level (the harness's step 3, part 3), judged against the
// faults in the history. Five nodes increment a counter under lost and duplicated messages and
// partitions, while nodes crash alone and a majority is killed together. `persist`: each keep
// is durable before it answers (`replicated`); otherwise each store answers from memory and
// syncs on its own every 200 to 600 ms (`decided`). A crashed node that lost what it had not
// synced votes only once caught up; a majority killed together comes back with what each had
// synced and votes at once, there being no majority left to catch up from. The history is then
// judged at `judged`: every committed increment whose loss the faults excuse may or may not have
// taken effect; any other must be there. `strict`, if given, says whether the history also
// held with nothing excused.
Run lasting(uint64_t seed, bool persist, AckLevel judged, bool* strict = nullptr) {
  World world(seed);
  NetworkFaults faults;
  faults.drop_one_in = 10;
  faults.duplicate_one_in = 10;
  faults.max_delay_ms = 30;
  Cluster cluster(world, 5, faults, persist);
  Invariants invariants(world, cluster, counter_order());
  const size_t majority = cluster.size() / 2 + 1;
  std::vector<Fault> log;
  const auto note = [&](std::string kind, std::vector<uint64_t> nodes) { log.push_back(Fault{world.now_ms(), std::move(kind), std::move(nodes), {}}); };
  std::function<void(size_t)> sync_later = [&](size_t i) {
    world.after(200 + world.random.below(400), [&, i] {
      if (cluster.up(i)) {
        cluster.store(i).sync();
        note("sync", {i});
      }
      if (world.now_ms() < 1'004'000) sync_later(i);
    });
  };
  if (!persist)
    for (size_t i = 0; i < cluster.size(); ++i) sync_later(i);
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
  // One node back: as it was, if each keep was durable; otherwise behind, catching up while a
  // majority of the others is up and voting, or later.
  std::function<void(size_t)> back = [&](size_t a) {
    if (cluster.up(a)) return;
    ++incarnation[a];
    if (persist) return cluster.restart(a);
    size_t voting = 0;
    for (size_t i = 0; i < cluster.size(); ++i)
      if (i != a && cluster.up(i) && cluster.store(i).ready()) ++voting;
    if (voting < majority) return world.after(100, [&, a] { back(a); });
    cluster.restart(a, false, false);
    catch_up(a, incarnation[a]);
  };
  const auto voting = [&] {
    size_t out = 0;
    for (size_t i = 0; i < cluster.size(); ++i)
      if (cluster.up(i) && cluster.store(i).ready()) ++out;
    return out;
  };
  // A node alone: with stores that sync later, only while a majority of the rest still votes,
  // so it can catch up. Separate crashes that leave a majority behind are a loss no rule
  // excuses, and nothing to catch up from: not this simulation's.
  const auto crash = [&](size_t a) {
    if (!cluster.up(a)) return;
    if (!persist && (!cluster.store(a).ready() || voting() - 1 < majority)) return;
    cluster.crash(a);
    note("kill", {a});
    // What it had not synced is gone, as declared: the invariants start it afresh.
    if (!persist) invariants.lost(a);
    world.after(100 + world.random.below(300), [&, a] { back(a); });
  };
  // A majority together, from a cluster that is whole.
  const auto kill_together = [&] {
    if (voting() < cluster.size()) return;
    std::vector<size_t> order;
    for (size_t i = 0; i < cluster.size(); ++i) order.push_back(i);
    for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[world.random.below(i)]);
    std::vector<uint64_t> killed;
    for (size_t i = 0; i < majority; ++i)
      if (cluster.up(order[i])) {
        cluster.crash(order[i]);
        killed.push_back(order[i]);
      }
    if (killed.empty()) return;
    note("kill", killed);
    const bool most = killed.size() >= majority;
    if (!persist) {
      if (most)
        invariants.excused_loss();
      else
        for (const auto k : killed) invariants.lost(k);
    }
    world.after(200, [&, killed, most] {
      for (const auto k : killed) {
        if (cluster.up(k)) continue;
        if (persist || !most) {
          back(k);
          continue;
        }
        ++incarnation[k];
        cluster.restart(k);
      }
    });
  };
  std::vector<Op> history(40);
  for (size_t n = 0; n < history.size(); ++n)
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
  for (int f = 0; f < 10; ++f)
    world.after(world.random.below(2000), [&] {
      const auto a = world.random.below(cluster.size());
      const auto b = world.random.below(cluster.size());
      switch (world.random.below(6)) {
        case 0:
          if (a != b) cluster.block(a, b);
          break;
        case 1:
          cluster.unblock_all();
          break;
        case 2:
        case 3:
          kill_together();
          break;
        default:
          crash(a);
          break;
      }
    });
  world.after(2500, [&] {
    cluster.unblock_all();
    cluster.faults().drop_one_in = 0;
    cluster.faults().duplicate_one_in = 0;
    for (size_t i = 0; i < cluster.size(); ++i) back(i);
  });
  const auto all_answered = [&] { return std::all_of(history.begin(), history.end(), [](const Op& op) { return op.answered || op.invoked == 0; }); };
  const auto all_voting = [&] {
    for (size_t i = 0; i < cluster.size(); ++i)
      if (!cluster.up(i) || !cluster.store(i).ready()) return false;
    return true;
  };
  Run run;
  if (!world.run_until([&] { return world.now_ms() >= 1'002'500 && all_answered() && all_voting(); }, 4'000'000)) {
    run.unsettled = true;
    run.violation = "the run did not settle";
    return finished(run, invariants);
  }
  std::optional<agree::Outcome> read;
  cluster.agreement(0).read(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, {}, 5000ms, world.io.get_executor(),
                            [&](agree::Outcome outcome) { read = std::move(outcome); });
  world.run_until([&] { return read.has_value(); }, 2'000'000);
  if (!read || !read->committed) {
    run.unsettled = true;
    run.violation = "the final read did not commit";
    return finished(run, invariants);
  }
  std::vector<Op> invoked;
  for (const auto& op : history)
    if (op.invoked) invoked.push_back(op);
  run.committed = static_cast<size_t>(std::count_if(invoked.begin(), invoked.end(), [](const Op& op) { return op.committed; }));
  if (strict) *strict = !check(invoked, decode(read->values[0]));
  // Judged at its level: a committed increment whose loss the faults excuse may not be there.
  for (auto& op : invoked)
    if (op.committed && loss_excused(log, judged, op.invoked, op.completed, 0, cluster.size())) op.committed = false;
  if (const auto refused = check(invoked, decode(read->values[0]))) run.violation = "durability: " + *refused;
  return finished(run, invariants);
}

uint64_t seeds(uint64_t fallback) {
  const char* set = std::getenv("DANNAN_SIM_SEEDS");
  return set && *set ? std::strtoull(set, nullptr, 10) : fallback;
}

// The first of `limit` seeds whose run a checker refuses: a refusal, never a run that merely
// failed to finish. Printed, so the record names the seed and what was found.
template <class Simulate>
std::optional<std::pair<uint64_t, std::string>> first_refused(Simulate simulate, uint64_t limit) {
  for (uint64_t seed = 1; seed <= limit; ++seed) {
    const auto run = simulate(seed);
    if (!run.violation || run.unsettled) continue;
    std::printf("[ refused  ] seed %llu: %s\n", static_cast<unsigned long long>(seed), run.violation->c_str());
    return std::pair{seed, *run.violation};
  }
  return std::nullopt;
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

// The checkers refuse each protocol broken on purpose (Miach's point: a verifier that has never
// refused anything is unverified), each with the seed that shows it.

// An acceptor that votes after losing its state can let a value already chosen be forgotten.
TEST(Simulation, TheCheckerCatchesAnAcceptorThatVotesAfterLosingItsState) {
  const auto caught = first_refused([](uint64_t seed) { return simulate(seed, Mode::broken); }, 400);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
}

// A proposer that skips prepare writes over a value chosen without having seen it.
TEST(Simulation, TheCheckersCatchAProposerThatSkipsPrepare) {
  const auto caught = first_refused([](uint64_t seed) { return simulate(seed, Mode::skip_prepare); }, 400);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
}

// An acceptor that loses track of its promises accepts, and promises, below one it gave: caught
// on the step it happens.
TEST(Simulation, TheCheckersCatchAnAcceptorThatAcceptsBelowItsPromise) {
  const auto caught = first_refused([](uint64_t seed) { return simulate(seed, Mode::forget_promises); }, 400);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
  EXPECT_NE(caught->second.find("invariant:"), std::string::npos) << caught->second;
  EXPECT_NE(caught->second.find("below the promise"), std::string::npos) << caught->second;
}

// A reduction that rolls the base back to an older answer's value loses what was chosen.
TEST(Simulation, TheCheckersCatchAReducerThatRollsTheBaseBack) {
  const auto caught = first_refused([](uint64_t seed) { return simulate(seed, Mode::rollback); }, 400);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
}

// A store whose sync reports success and keeps nothing, trusted by a node that restarts on it,
// comes back having forgotten what it promised.
TEST(Simulation, TheCheckersCatchALyingSyncThatIsTrusted) {
  const auto caught = first_refused([](uint64_t seed) { return simulate(seed, Mode::lying_sync); }, 400);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
}

// Two proposers whose rosters put the slot on different sets, one of them without the proof
// that its set serves the slot (the reconfiguration rule), choose on two majorities that need not
// meet.
TEST(Simulation, TheCheckersCatchProposersWhoseRostersNameDifferentSets) {
  const auto caught = first_refused([](uint64_t seed) { return simulate(seed, Mode::other_roster); }, 400);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
}

// Each level's rule, on faults written out by hand.
TEST(Durability, EachLevelIsExcusedOnlyByWhatLosesIt) {
  const std::vector<Fault> kill_one{{20, "kill", {1}, {}}};
  EXPECT_TRUE(loss_excused(kill_one, AckLevel::ephemeral, 10, 15, 1, 5));
  EXPECT_FALSE(loss_excused(kill_one, AckLevel::ephemeral, 10, 15, 2, 5));
  EXPECT_FALSE(loss_excused(kill_one, AckLevel::persisted, 10, 15, 1, 5));
  EXPECT_TRUE(loss_excused({{20, "wipe", {1}, {}}}, AckLevel::persisted, 10, 15, 1, 5));
  // Decided: a majority killed together, none of them synced since the answer.
  const std::vector<Fault> three{{30, "kill", {0, 1, 2}, {}}};
  EXPECT_TRUE(loss_excused(three, AckLevel::decided, 10, 15, 0, 5));
  EXPECT_FALSE(loss_excused(three, AckLevel::replicated, 10, 15, 0, 5));
  // One of them synced after the answer: a majority no longer lost it.
  std::vector<Fault> synced = three;
  synced.insert(synced.begin(), Fault{20, "sync", {1}, {}});
  EXPECT_FALSE(loss_excused(synced, AckLevel::decided, 10, 15, 0, 5));
  // A sync before the answer may not hold the write: it does not count.
  std::vector<Fault> early = three;
  early.insert(early.begin(), Fault{12, "sync", {1}, {}});
  EXPECT_TRUE(loss_excused(early, AckLevel::decided, 10, 15, 0, 5));
  // A kill before the write was invoked excuses nothing; two killed alone neither.
  EXPECT_FALSE(loss_excused({{5, "kill", {0, 1, 2}, {}}}, AckLevel::decided, 10, 15, 0, 5));
  EXPECT_FALSE(loss_excused({{20, "kill", {0}, {}}, {21, "kill", {1}, {}}, {22, "kill", {2}, {}}}, AckLevel::decided, 10, 15, 0, 5));
  // Replicated: a majority's disks.
  EXPECT_FALSE(loss_excused({{20, "wipe", {0, 1}, {}}}, AckLevel::replicated, 10, 15, 0, 5));
  EXPECT_TRUE(loss_excused({{20, "wipe", {0, 1}, {}}, {25, "wipe", {2}, {}}}, AckLevel::replicated, 10, 15, 0, 5));
}

// Replicated: every keep durable before it answers, so a majority killed together loses
// nothing, and the history holds with nothing excused.
TEST(Simulation, AReplicatedCommitSurvivesAMajorityKilledTogether) {
  size_t committed = 0;
  for (uint64_t seed = 1; seed <= seeds(60); ++seed) {
    const auto run = lasting(seed, true, AckLevel::replicated);
    ASSERT_FALSE(run.violation) << "seed " << seed << ": " << *run.violation;
    committed += run.committed;
  }
  EXPECT_GT(committed, 0u);
}

// Decided: stores answer from memory and sync later. A majority killed together before syncing
// may lose commits, and some seed does (the history fails with nothing excused); every loss is
// one the faults excuse, and nothing else is lost.
TEST(Simulation, ADecidedCommitIsLostOnlyWhenAMajorityStopsBeforeSyncing) {
  size_t lost = 0;
  for (uint64_t seed = 1; seed <= seeds(60); ++seed) {
    bool strict = true;
    const auto run = lasting(seed, false, AckLevel::decided, &strict);
    ASSERT_FALSE(run.violation) << "seed " << seed << ": " << *run.violation;
    if (!strict) ++lost;
  }
  EXPECT_GT(lost, 0u) << "no seed lost a decided commit: the rule was never exercised";
}

// The broken protocol: stores that answer from memory, taken for replicated (a durable answer
// that was not). A majority killed together loses commits the faults do not excuse at that level.
TEST(Simulation, TheCheckersCatchADecidedStoreTakenForReplicated) {
  const auto caught = first_refused([](uint64_t seed) { return lasting(seed, false, AckLevel::replicated); }, 200);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
  EXPECT_NE(caught->second.find("durability:"), std::string::npos) << caught->second;
}

// Ruling 16 kept: with the union of every promiser's merged state, every command sees every merge
// write acknowledged at `replicated` before it began, and the invariants hold for the merge class.
TEST(Simulation, EveryCommandSeesTheMergeWritesAcknowledgedBeforeIt) {
  size_t committed = 0;
  for (uint64_t seed = 1; seed <= seeds(40); ++seed) {
    const auto run = merge_class(seed, false);
    ASSERT_FALSE(run.violation) << "seed " << seed << ": " << *run.violation;
    committed += run.committed;
  }
  EXPECT_GT(committed, 0u);
}

// The default reduction, the base's value alone, drops a merge write a majority holds but the
// base's holder does not.
TEST(Simulation, TheCheckersCatchAReducerThatDropsAReplicatedMergeWrite) {
  const auto caught = first_refused([](uint64_t seed) { return merge_class(seed, true); }, 400);
  ASSERT_TRUE(caught) << "no seed showed the broken protocol: the checkers are unproven";
  EXPECT_NE(caught->second.find("merge:"), std::string::npos) << caught->second;
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

// An append that also returns its delta: the byte appended (the simulator's store applies a
// delta by appending it).
agree::Change append_with_delta(uint8_t byte) {
  return [byte](std::span<const std::optional<agree::Bytes>> current) {
    auto next = current[0].value_or(agree::Bytes{});
    next.push_back(byte);
    return agree::Changed{{next}, encode(next.size()), {agree::Bytes{byte}}};
  };
}

// Deltas through the runtime: to an acceptor holding the base only the delta crosses the
// network, applied in place by its store; one that missed the base is sent the whole value.
// When a store's own check refuses every delta, the names go whole and the change still
// commits. Every acceptor ends with the same value.
TEST(Simulation, ADeltaIsAppliedInPlaceAndAnAcceptorBehindIsSentTheWholeValue) {
  World world(29);
  Cluster cluster(world, 3, NetworkFaults{});
  const auto change_at = [&](size_t at, agree::Change change) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change), {}, 5000ms,
                                 world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 5'000'000);
    // Let every accept still in flight land.
    world.run_until([] { return false; }, 200'000);
    return out.value_or(agree::Outcome{});
  };
  const agree::Bytes megabyte(1024 * 1024, 'a');
  ASSERT_TRUE(change_at(0, put(megabyte)).committed);
  // Every acceptor holds the base: the other promiser is sent only the delta.
  cluster.recording = true;
  ASSERT_TRUE(change_at(1, append_with_delta('b')).committed);
  const auto small_accept = std::any_of(cluster.crossed.begin(), cluster.crossed.end(),
                                        [](const Cluster::Crossed& each) { return each.type == net::message::agree_accept && each.request_bytes < 4096; });
  EXPECT_TRUE(small_accept);
  // Node 2 misses the next change, then changes the register itself: its own acceptor is
  // behind the base, so it is sent the whole value. The next change is node 1's, which took
  // the last: a fast-path miss with node 2 silent would end unknown, rightly (node 2 might have
  // taken it and been built on), which is not what this test is about.
  cluster.block(1, 2);
  ASSERT_TRUE(change_at(1, append_with_delta('c')).committed);
  cluster.unblock_all();
  const auto changed = change_at(2, append_with_delta('d'));
  ASSERT_TRUE(changed.committed) << changed.why;
  EXPECT_EQ(decode(changed.result), megabyte.size() + 3);
  for (size_t at = 0; at < 3; ++at) {
    const auto held = cluster.store(at).state(counter_slot, agree::Name{'m'});
    if (!held || held->accepted != changed.ballot) continue;
    ASSERT_TRUE(held->value);
    EXPECT_EQ(held->value->size(), megabyte.size() + 3) << "at " << at;
    EXPECT_EQ(held->value->back(), 'd');
  }

  // Every store now refuses deltas: they go whole, and the change commits.
  for (size_t at = 0; at < 3; ++at) cluster.store(at).refuse_deltas = true;
  const auto whole = change_at(1, append_with_delta('e'));
  ASSERT_TRUE(whole.committed) << whole.why;
  EXPECT_EQ(decode(whole.result), megabyte.size() + 4);
}

// The value-less change through the runtime (part 6): a delta on a megabyte is committed with
// no value fetched or sent; the next takes the fast path, its summary checked; a read with no
// delta commits with no accept; an acceptor lacking the base is named, caught up by an
// identity change(), and counted again after; a declined change ends at once.
TEST(Simulation, AValueLessChangeCarriesOnlyDeltas) {
  World world(37);
  Cluster cluster(world, 3, NetworkFaults{});
  const auto settle = [&] { world.run_until([] { return false; }, 200'000); };
  const auto deltas_at = [&](size_t at, agree::DeltaChange change, std::chrono::milliseconds deadline = 5000ms) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change_deltas(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change), deadline,
                                        world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 5'000'000);
    settle();
    return out.value_or(agree::Outcome{});
  };
  const auto whole_at = [&](size_t at, agree::Change change) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change), {}, 5000ms,
                                 world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 5'000'000);
    settle();
    return out.value_or(agree::Outcome{});
  };
  // The simulator's store summarises a value by its length and applies a delta by appending,
  // so a change knows exactly the summary its commit leaves.
  const auto append = [](uint8_t byte) {
    return agree::DeltaChange([byte](std::span<const agree::Base> bases) {
      const auto length = bases[0].has_value ? decode(std::optional<agree::Bytes>(bases[0].summary)) : 0;
      return agree::Deltas{{agree::Bytes{byte}}, encode(length + 1), false, {encode(length + 1)}};
    });
  };
  const agree::Bytes megabyte(1024 * 1024, 'v');
  ASSERT_TRUE(whole_at(0, put(megabyte)).committed);

  cluster.recording = true;
  bool local = false;
  const auto first = deltas_at(1, [&](std::span<const agree::Base> bases) {
    local = bases[0].local;
    return agree::Deltas{{agree::Bytes{'a'}}, encode(megabyte.size() + 1), false, {encode(megabyte.size() + 1)}};
  });
  ASSERT_TRUE(first.committed) << first.why;
  EXPECT_TRUE(local);
  EXPECT_TRUE(first.values.empty());
  EXPECT_FALSE(a_prepare_answer_carried_a_value(cluster));
  EXPECT_EQ(crossed_of(cluster, net::message::agree_fetch), 0u);
  for (const auto& each : cluster.crossed) {
    if (each.type == net::message::agree_accept) {
      EXPECT_LT(each.request_bytes, 4096u);
    }
  }
  const auto fast = deltas_at(1, append('b'));
  ASSERT_TRUE(fast.committed) << fast.why;
  EXPECT_TRUE(fast.fast);
  EXPECT_EQ(decode(fast.result), megabyte.size() + 2);

  // A read with no delta: one round, no accept.
  cluster.crossed.clear();
  const agree::DeltaChange none = [](std::span<const agree::Base> bases) {
    return agree::Deltas{std::vector<std::optional<agree::Bytes>>(bases.size()), agree::Bytes(bases[0].summary)};
  };
  const auto read = deltas_at(2, none);
  ASSERT_TRUE(read.committed) << read.why;
  EXPECT_EQ(decode(read.result), megabyte.size() + 2);
  EXPECT_EQ(crossed_of(cluster, net::message::agree_accept), 0u);

  // Node 2 misses a change, and node 1 goes down: a majority now needs node 2, which lacks
  // the base. It is named, and the change cannot commit. (Node 0 has no fast path cached, so
  // it goes the full way at once.)
  cluster.block(1, 2);
  ASSERT_TRUE(deltas_at(1, append('c')).committed);
  cluster.unblock_all();
  cluster.crash(1);
  const auto stuck = deltas_at(0, append('d'), 800ms);
  EXPECT_FALSE(stuck.committed);
  EXPECT_EQ(stuck.need_whole, cluster.ids({2}));
  // An identity change() sends node 2 the whole value; then the value-less change commits.
  const auto caught = whole_at(0, [](std::span<const std::optional<agree::Bytes>> current) {
    return agree::Changed{std::vector<std::optional<agree::Bytes>>(current.begin(), current.end()), {}, {}, true};
  });
  ASSERT_TRUE(caught.committed) << caught.why;
  const auto after = deltas_at(0, append('e'));
  ASSERT_TRUE(after.committed) << after.why;
  EXPECT_TRUE(after.need_whole.empty());
  // 'd', accepted by node 0 alone, may have been carried on by the catch-up, as Paxos allows.
  const auto held = cluster.store(2).state(counter_slot, agree::Name{'m'});
  ASSERT_TRUE(held && held->value);
  EXPECT_EQ(decode(after.result), held->value->size());
  EXPECT_GE(held->value->size(), megabyte.size() + 4);
  EXPECT_EQ(held->value->back(), 'e');

  // Declined: at once, nothing sent.
  const auto declined = deltas_at(0, [](std::span<const agree::Base>) { return agree::Deltas{{}, {}, true}; });
  EXPECT_FALSE(declined.committed);
  EXPECT_EQ(declined.why, "declined");
  EXPECT_EQ(declined.attempts, 1u);
}

// Kvasir's ranges: a transfer whose new acceptors were caught up beforehand by the store's own
// means sends no value at all. Every prepare answer is value-less, every accept carries the
// ballot only, and the new acceptors hold the value at the transfer's ballot.
TEST(Simulation, ATransferWhoseNewAcceptorsHoldTheValuesSendsNoValue) {
  World world(31);
  Cluster cluster(world, 5, NetworkFaults{}, true, 3);
  const auto told = [&](auto start) {
    std::optional<agree::Outcome> out;
    start([&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 5'000'000);
    world.run_until([] { return false; }, 200'000);
    return out.value_or(agree::Outcome{});
  };
  constexpr agree::Slot moved_slot = 1;
  const agree::Bytes megabyte(1024 * 1024, 'r');
  const auto written = told([&](auto done) {
    cluster.agreement(0).change(Cluster::space, moved_slot, {agree::Name{'m'}}, cluster.ids({0, 1, 2}), 0, put(megabyte), {}, 5000ms, world.io.get_executor(),
                                done);
  });
  ASSERT_TRUE(written.committed) << written.why;
  // Nodes 3 and 4 copy the range from node 0, as a store's own catch-up would.
  const auto state = cluster.store(0).state(moved_slot, agree::Name{'m'});
  ASSERT_TRUE(state);
  for (const size_t at : {3, 4}) {
    agree::Held copy;
    copy.names = {agree::Name{'m'}};
    copy.states = {*state};
    cluster.store(at).keep(moved_slot, copy, [](Store::Kept kept) { ASSERT_TRUE(kept.kept); });
  }
  cluster.recording = true;
  const auto moved = told([&](auto done) {
    cluster.agreement(2).transfer(Cluster::space, moved_slot, cluster.ids({0, 1, 2}), 0, cluster.ids({2, 3, 4}), 1, 5000ms, world.io.get_executor(), done);
  });
  ASSERT_TRUE(moved.committed) << moved.why;
  EXPECT_FALSE(a_prepare_answer_carried_a_value(cluster));
  for (const auto& each : cluster.crossed)
    if (each.type == net::message::agree_accept) {
      EXPECT_LT(each.request_bytes, 64u * 1024) << "an accept to node " << each.to;
    }
  for (const size_t at : {3, 4}) {
    const auto held = cluster.store(at).state(moved_slot, agree::Name{'m'});
    ASSERT_TRUE(held);
    EXPECT_EQ(held->accepted, moved.ballot);
    ASSERT_TRUE(held->value);
    EXPECT_EQ(*held->value, megabyte);
  }
}

// Large values and deltas under faults: lost, duplicated and delayed messages, and a crash
// and restart. Every append is a delta on a megabyte; a duplicated or replayed accept must
// never apply one twice, and every committed append is where its result says.
TEST(Simulation, LargeValuesAndDeltasStayConsistentUnderFaults) {
  for (uint64_t seed = 1; seed <= seeds(8); ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    World world(seed);
    Cluster cluster(world, 3, NetworkFaults{20, 20, 20});
    const auto change_at = [&](size_t at, agree::Change change) {
      std::optional<agree::Outcome> out;
      cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change), {}, 5000ms,
                                   world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
      world.run_until([&] { return out.has_value(); }, 5'000'000);
      return out.value_or(agree::Outcome{});
    };
    const agree::Bytes megabyte(1024 * 1024, '.');
    bool based = false;
    for (int tries = 0; tries < 5 && !based; ++tries) based = change_at(0, put(megabyte)).committed;
    ASSERT_TRUE(based);
    std::vector<std::pair<uint64_t, uint8_t>> committed;
    for (uint8_t i = 0; i < 24; ++i) {
      if (i == 12) {
        cluster.crash(1);
        cluster.restart(1);
      }
      const uint8_t byte = static_cast<uint8_t>('A' + i);
      const auto outcome = change_at(i % 3, append_with_delta(byte));
      if (outcome.committed) committed.emplace_back(decode(outcome.result), byte);
    }
    cluster.faults() = NetworkFaults{};
    const auto read = change_at(0, [](std::span<const std::optional<agree::Bytes>> current) { return agree::Changed{{current[0]}, {}, {}, true}; });
    ASSERT_TRUE(read.committed) << read.why;
    ASSERT_TRUE(read.values[0]);
    const auto& value = *read.values[0];
    ASSERT_GE(value.size(), megabyte.size());
    EXPECT_TRUE(std::equal(megabyte.begin(), megabyte.end(), value.begin()));
    // Each append once at most, in the place its result names.
    std::set<uint8_t> seen;
    for (size_t at = megabyte.size(); at < value.size(); ++at) EXPECT_TRUE(seen.insert(value[at]).second) << "applied twice: " << value[at];
    for (const auto& [length, byte] : committed) {
      ASSERT_LE(length, value.size());
      EXPECT_EQ(value[length - 1], byte);
    }
    EXPECT_FALSE(committed.empty());
  }
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

// Liveness with one acceptor silent (the maintainer, 2026-11-17: "watch that commit fail"). Node 0
// commits while node 2 hears nothing from it, reserving its next ballot at acceptors 0 and 1
// (the fast path), and falls silent. Node 2's first prepare is promised by its own acceptor,
// which missed the reservation, refused by acceptor 1 for it, and never answered by 0: neither
// a majority promising nor a majority refusing. Nodes 1 and 2 are a live majority, and a retry
// above the reservation commits. The change must not wait for the silent node until its own
// deadline.
TEST(Simulation, AChangeRefusedByOneAcceptorAndUnansweredByAnotherRetriesWithinItsDeadline) {
  World world(3);
  NetworkFaults quiet;
  quiet.max_delay_ms = 1;
  Cluster cluster(world, 3, quiet);
  const auto change_at = [&](size_t at, agree::Change change) {
    std::optional<agree::Outcome> out;
    cluster.agreement(at).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, std::move(change), {}, 5000ms,
                                 world.io.get_executor(), [&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 2'000'000);
    return out.value_or(agree::Outcome{});
  };
  cluster.block(0, 2);
  const auto written = change_at(0, put(agree::Bytes{'b', 'a', 's', 'e'}));
  ASSERT_TRUE(written.committed) << written.why;
  world.run_until([] { return false; }, 200'000);
  ASSERT_FALSE(cluster.store(2).state(counter_slot, agree::Name{'m'}));
  cluster.unblock_all();
  cluster.crash(0);
  const auto begun = world.now_ms();
  const auto changed = change_at(2, append('!'));
  EXPECT_TRUE(changed.committed) << changed.why;
  // Well inside the deadline: a few rounds, not the five seconds.
  EXPECT_LT(world.now_ms() - begun, 1000u) << changed.attempts << " attempts: " << changed.why;
}

// The same for a fetch: the base is on nodes 0, 1 and 2, node 4 cannot reach 0, and node 1, the
// first holder node 4 fetches the base from, dies the moment it promises. Node 2 holds the same
// base: the fetch moves to it after a round rather than waiting on 1 until the deadline.
TEST(Simulation, AFetchWhoseHolderFallsSilentMovesToTheNextHolderWithinItsDeadline) {
  World world(3);
  NetworkFaults quiet;
  quiet.max_delay_ms = 1;
  Cluster cluster(world, 5, quiet);
  std::optional<agree::Outcome> written;
  cluster.block(0, 3);
  cluster.block(0, 4);
  cluster.agreement(0).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, put(agree::Bytes{'b', 'a', 's', 'e'}),
                              {}, 5000ms, world.io.get_executor(), [&](agree::Outcome outcome) { written = std::move(outcome); });
  ASSERT_TRUE(world.run_until([&] { return written.has_value(); }, 2'000'000));
  ASSERT_TRUE(written->committed) << written->why;
  world.run_until([] { return false; }, 200'000);
  cluster.unblock_all();
  ASSERT_FALSE(cluster.store(4).state(counter_slot, agree::Name{'m'}));

  cluster.block(4, 0);
  std::optional<agree::Outcome> changed;
  const auto begun = world.now_ms();
  const auto before = cluster.store(1).changes();
  cluster.agreement(4).change(Cluster::space, counter_slot, {agree::Name{'m'}}, cluster.ids(), Cluster::genesis_version, append('!'), {}, 5000ms,
                              world.io.get_executor(), [&](agree::Outcome outcome) { changed = std::move(outcome); });
  // Node 1 keeps its promise, and dies with the answer on its way.
  ASSERT_TRUE(world.run_until([&] { return changed.has_value() || cluster.store(1).changes() != before; }, 2'000'000));
  ASSERT_FALSE(changed.has_value()) << changed->why;
  cluster.crash(1);
  ASSERT_TRUE(world.run_until([&] { return changed.has_value(); }, 2'000'000));
  EXPECT_TRUE(changed->committed) << changed->why;
  EXPECT_LT(world.now_ms() - begun, 2000u) << changed->attempts << " attempts: " << changed->why;
  const auto held = cluster.store(2).state(counter_slot, agree::Name{'m'});
  ASSERT_TRUE(held && held->value);
  EXPECT_EQ(std::string(held->value->begin(), held->value->end()), "base!");
}

// A change is applied once (the review of 2026-11-17, finding 1). The counter cannot show a
// change applied twice while changes that did not commit may account for it, so here every
// change appends a byte of its own: under the same faults as the counter's runs, no byte is in
// the register twice.
TEST(Simulation, NoChangeIsAppliedTwiceUnderFaults) {
  size_t twice = 0;
  // How the changes ended, over every seed: what a hosted system meets under these faults.
  size_t committed = 0, unknown = 0, ambiguous = 0, failed = 0;
  uint64_t first_seed = 0;
  for (uint64_t seed = 1; seed <= seeds(150); ++seed) {
    World world(seed);
    NetworkFaults faults;
    faults.drop_one_in = 8;
    faults.duplicate_one_in = 10;
    faults.max_delay_ms = 30;
    Cluster cluster(world, 5, faults);
    const agree::Name name{'l'};
    size_t answered = 0, invoked = 0;
    for (uint8_t n = 0; n < 40; ++n)
      world.after(world.random.below(2000), [&, n] {
        size_t at = world.random.below(cluster.size());
        for (size_t tries = 0; tries < cluster.size() && !cluster.up(at); ++tries) at = (at + 1) % cluster.size();
        if (!cluster.up(at)) return;
        ++invoked;
        cluster.agreement(at).change(Cluster::space, counter_slot, {name}, cluster.ids(), Cluster::genesis_version, append(static_cast<uint8_t>('0' + n)), {},
                                     3000ms, world.io.get_executor(), [&](agree::Outcome outcome) {
                                       ++answered;
                                       ++(outcome.committed ? committed : outcome.unknown ? unknown : failed);
                                       if (outcome.unknown && outcome.why.find("earlier accept") != std::string::npos) ++ambiguous;
                                     });
      });
    for (int f = 0; f < 12; ++f)
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
            if (!cluster.up(a)) cluster.restart(a);
            break;
        }
      });
    world.after(2500, [&] {
      cluster.unblock_all();
      cluster.faults() = NetworkFaults{};
      for (size_t i = 0; i < cluster.size(); ++i)
        if (!cluster.up(i)) cluster.restart(i);
    });
    ASSERT_TRUE(world.run_until([&] { return world.now_ms() >= 1'002'500 && answered == invoked; }, 2'000'000)) << "seed " << seed;
    std::optional<agree::Outcome> read;
    cluster.agreement(0).read(Cluster::space, counter_slot, {name}, cluster.ids(), Cluster::genesis_version, {}, 5000ms, world.io.get_executor(),
                              [&](agree::Outcome outcome) { read = std::move(outcome); });
    world.run_until([&] { return read.has_value(); }, 2'000'000);
    ASSERT_TRUE(read && read->committed) << "seed " << seed;
    const auto value = read->values[0].value_or(agree::Bytes{});
    std::set<uint8_t> seen;
    bool again = false;
    for (const auto byte : value) again = !seen.insert(byte).second || again;
    if (again) {
      if (++twice == 1) {
        first_seed = seed;
        std::printf("[ twice    ] seed %llu: %s\n", static_cast<unsigned long long>(seed), std::string(value.begin(), value.end()).c_str());
      }
    }
  }
  std::printf("[ outcomes ] committed %zu, unknown %zu (%zu of them a retry that could not tell), failed and not taken %zu\n", committed, unknown, ambiguous,
              failed);
  EXPECT_EQ(twice, 0u) << "seeds with a change applied twice, the first seed " << first_seed;
  EXPECT_GT(committed, 0u);
}

// Deleting a name through the runtime (the review of 2026-11-17, finding 3): with every replica
// up and the network clean, forget() commits the tombstone, sees every replica hold it, and
// purges it everywhere.
TEST(Simulation, AForgottenNameIsPurgedAtEveryReplica) {
  World world(41);
  Cluster cluster(world, 3, NetworkFaults{});
  const agree::Name name{'d'};
  std::optional<agree::Outcome> written;
  cluster.agreement(0).change(Cluster::space, counter_slot, {name}, cluster.ids(), Cluster::genesis_version, put(agree::Bytes{'x'}), {}, 5000ms,
                              world.io.get_executor(), [&](agree::Outcome outcome) { written = std::move(outcome); });
  ASSERT_TRUE(world.run_until([&] { return written.has_value(); }, 2'000'000));
  ASSERT_TRUE(written->committed) << written->why;
  world.run_until([] { return false; }, 200'000);
  std::optional<agree::Outcome> forgotten;
  cluster.agreement(1).forget(Cluster::space, counter_slot, {name}, cluster.ids(), Cluster::genesis_version, 5000ms, world.io.get_executor(),
                              [&](agree::Outcome outcome) { forgotten = std::move(outcome); });
  ASSERT_TRUE(world.run_until([&] { return forgotten.has_value(); }, 2'000'000));
  ASSERT_TRUE(forgotten->committed) << forgotten->why;
  EXPECT_TRUE(forgotten->forgotten) << forgotten->why;
  for (size_t at = 0; at < cluster.size(); ++at) EXPECT_FALSE(cluster.store(at).state(counter_slot, name)) << "node " << at << " still holds the name";
}

// Finding 6: a node that lost its state and caught up keeps a forgotten name forgotten. The name
// is purged everywhere, node 2 is wiped and catches up on another name, and a stale accept for
// the forgotten one, below the floor every replica held, reaches it. It is refused, and a read
// that node 2's own acceptor answers first still finds nothing.
TEST(Simulation, ANodeCaughtUpAfterLosingItsStateRefusesAStaleAcceptForAForgottenName) {
  World world(43);
  Cluster cluster(world, 3, NetworkFaults{});
  const agree::Name gone{'d'};
  const auto told = [&](auto start) {
    std::optional<agree::Outcome> out;
    start([&](agree::Outcome outcome) { out = std::move(outcome); });
    world.run_until([&] { return out.has_value(); }, 2'000'000);
    world.run_until([] { return false; }, 200'000);
    return out.value_or(agree::Outcome{});
  };
  ASSERT_TRUE(told([&](auto done) {
                cluster.agreement(0).change(Cluster::space, counter_slot, {gone}, cluster.ids(), Cluster::genesis_version, put(agree::Bytes{'x'}), {}, 5000ms,
                                            world.io.get_executor(), done);
              }).committed);
  const auto forgotten = told([&](auto done) {
    cluster.agreement(0).forget(Cluster::space, counter_slot, {gone}, cluster.ids(), Cluster::genesis_version, 5000ms, world.io.get_executor(), done);
  });
  ASSERT_TRUE(forgotten.forgotten) << forgotten.why;
  cluster.crash(2);
  cluster.restart(2, true, false);
  const auto caught = told([&](auto done) {
    cluster.agreement(2).catch_up(Cluster::space, counter_slot, {agree::Name{'c'}}, cluster.ids(), Cluster::genesis_version, 5000ms, world.io.get_executor(),
                                  done);
  });
  ASSERT_TRUE(caught.committed) << caught.why;
  cluster.store(2).set_ready(true);
  // The stale accept: the value before the tombstone, at the lowest ballot, from node 0.
  agree::Accept stale;
  stale.set = cluster.genesis();
  stale.slot = counter_slot;
  stale.ballot = agree::Ballot{1, cluster.id(0), {}};
  stale.names = {gone};
  stale.values = {agree::Bytes{'x'}};
  stale.changed_at = {stale.ballot};
  std::optional<net::Bytes> answer;
  cluster.call(0, 2, net::message::agree_accept,
               agree::encode(agree::AcceptMessage{Cluster::space, stale, agree::table_fingerprint(Cluster::slots, cluster.genesis())}), 500ms,
               [&](std::optional<net::Bytes> reply, std::string) { answer = reply.value_or(net::Bytes{}); });
  ASSERT_TRUE(world.run_until([&] { return answer.has_value(); }, 2'000'000));
  ASSERT_FALSE(answer->empty());
  EXPECT_TRUE(agree::decode_accept_answer(*answer).refused) << "the stale accept was taken";
  const auto read = told([&](auto done) {
    cluster.agreement(2).read(Cluster::space, counter_slot, {gone}, cluster.ids(), Cluster::genesis_version, {}, 5000ms, world.io.get_executor(), done);
  });
  ASSERT_TRUE(read.committed) << read.why;
  EXPECT_FALSE(read.values[0]) << "the forgotten value came back";
}

// Finding 7 (2026-11-17): a change of several names can be half taken, since each name's base
// is chosen on its own (proven at the core: one name adopted half of an accept that another
// passed over). Until plan item 7's atomic commit, the runtime takes one name per change, so no
// accept of several names is ever made to be half taken (Mimir's ask: refused, not documented).
// A read may name several: its accept changes nothing, half taken or whole.
TEST(Agreement, AChangeOfSeveralNamesIsNeverHalfTaken) {
  World world(47);
  Cluster cluster(world, 3, NetworkFaults{});
  const std::vector<agree::Name> both{agree::Name{'x'}, agree::Name{'y'}};
  const auto ignore = [](agree::Outcome) {};
  EXPECT_THROW(cluster.agreement(0).change(Cluster::space, counter_slot, both, cluster.ids(), Cluster::genesis_version, put(agree::Bytes{'v'}), {}, 1000ms,
                                           world.io.get_executor(), ignore),
               std::invalid_argument);
  EXPECT_THROW(cluster.agreement(0).change_deltas(
                   Cluster::space, counter_slot, both, cluster.ids(), Cluster::genesis_version,
                   [](std::span<const agree::Base> bases) { return agree::Deltas{std::vector<std::optional<agree::Bytes>>(bases.size()), {}}; }, 1000ms,
                   world.io.get_executor(), ignore),
               std::invalid_argument);
  EXPECT_THROW(
      cluster.agreement(0).forget(Cluster::space, counter_slot, both, cluster.ids(), Cluster::genesis_version, 1000ms, world.io.get_executor(), ignore),
      std::invalid_argument);
  std::optional<agree::Outcome> read;
  cluster.agreement(0).read(Cluster::space, counter_slot, both, cluster.ids(), Cluster::genesis_version, {}, 1000ms, world.io.get_executor(),
                            [&](agree::Outcome outcome) { read = std::move(outcome); });
  ASSERT_TRUE(world.run_until([&] { return read.has_value(); }, 2'000'000));
  EXPECT_TRUE(read->committed) << read->why;
  EXPECT_EQ(read->values.size(), 2u);
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
