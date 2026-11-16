//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Agreement's pure core (TODO/2026-11-15-agreement-design.md): the acceptor's rules and
// one proposer's attempt, driven by hand and by a seeded simulator that delivers, drops
// and reorders every message, with no network, clock or sleep.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dannan/agree/caspaxos.h"
#include "dannan/agree/wire.h"

namespace dannan::agree {
namespace {

identity::NodeId node(uint8_t n) {
  identity::NodeId id;
  id.bytes[15] = n;
  return id;
}

Bytes bytes(const std::string& text) { return Bytes(text.begin(), text.end()); }
std::string text(const Bytes& b) { return std::string(b.begin(), b.end()); }

Ballot ballot(uint64_t counter, uint8_t by) { return Ballot{counter, node(by), node(static_cast<uint8_t>(100 + by))}; }

const ReplicaSet three = ReplicaSet::of({node(1), node(2), node(3)}, 1);
// The genesis set of these tests, which serves a slot no acceptor has recorded.
const ReplicaSet& genesis = three;
constexpr Slot slot = 3;

// The acceptors' state, kept as a hosted system would, and the rules applied to it. Keyed
// by the name's bytes as a string: GCC 14 at -O3 misreads a map of byte vectors' compare.
struct Acceptors {
  std::map<identity::NodeId, std::map<std::string, AcceptorState>> held;
  std::map<identity::NodeId, SlotState> slots;
  std::map<identity::NodeId, Ballot> floor;
  ReplicaSet founders = genesis;

  static std::string key(const Name& name) { return std::string(name.begin(), name.end()); }
  // The request's names, then, for a transfer, every other name held.
  Held load(const identity::NodeId& at, const std::vector<Name>& names, bool every) {
    Held out{slots[at], floor[at], {}, {}};
    for (const auto& name : names) {
      out.names.push_back(name);
      out.states.push_back(held[at][key(name)]);
    }
    if (every)
      for (const auto& [name, state] : held[at])
        if (std::find(names.begin(), names.end(), bytes(name)) == names.end()) {
          out.names.push_back(bytes(name));
          out.states.push_back(state);
        }
    return out;
  }
  // A delta here is bytes appended to the value held (the rule has checked its base).
  void keep(const identity::NodeId& at, const Held& kept) {
    slots[at] = kept.slot;
    floor[at] = kept.floor;
    for (size_t i = 0; i < kept.names.size(); ++i) {
      auto state = kept.states[i];
      auto& stored = held[at][key(kept.names[i])];
      if (state.delta) {
        auto value = stored.value.value_or(Bytes{});
        value.insert(value.end(), state.delta->begin(), state.delta->end());
        state.value = std::move(value);
        state.delta.reset();
        state.base = {};
      } else if (state.has_value && !state.value) {
        state.value = stored.value;
      }
      stored = std::move(state);
    }
  }
  PrepareAnswer prepare(const identity::NodeId& at, const identity::NodeId& from, const Prepare& request) {
    auto step = on_prepare(at, from, request, load(at, request.names, request.to.has_value()), founders);
    if (step.keep) keep(at, *step.keep);
    return step.answer;
  }
  AcceptAnswer accept(const identity::NodeId& at, const identity::NodeId& from, const Accept& request) {
    auto step = on_accept(at, from, request, load(at, request.names, request.to.has_value()), founders);
    if (step.keep) keep(at, *step.keep);
    return step.answer;
  }
  AcceptAnswer settle(const identity::NodeId& at, const identity::NodeId& from, const Settle& request) {
    auto step = on_settle(at, from, request, load(at, {}, false));
    if (step.keep) keep(at, *step.keep);
    return step.answer;
  }
  PurgeStep purge(const identity::NodeId& at, const identity::NodeId& from, const Purge& request) {
    auto step = on_purge(at, from, request, load(at, request.names, false));
    if (!step.refused) keep(at, step.keep);
    return step;
  }
};

// Drives an attempt to its end: prepares reach `prepared` (every recipient, if empty) in
// order, accepts reach `accepting` (every recipient, if empty).
Attempt& drive(Acceptors& acceptors, Attempt& attempt, const identity::NodeId& from, std::vector<identity::NodeId> prepared = {},
               std::vector<identity::NodeId> accepting = {}) {
  if (prepared.empty()) prepared = attempt.recipients();
  if (accepting.empty()) accepting = attempt.recipients();
  std::optional<Accept> accept;
  for (const auto& at : prepared)
    if (auto next = attempt.on_prepare(at, acceptors.prepare(at, from, attempt.prepare()))) accept = next;
  if (accept)
    for (const auto& at : accepting) attempt.on_accept(at, acceptors.accept(at, from, *accept));
  return attempt;
}

// Runs one plain attempt to its end with every replica answering in order.
Attempt run(Acceptors& acceptors, const ReplicaSet& set, const identity::NodeId& from, Ballot b, std::vector<Name> names, Change change, Reduce reduce = {}) {
  Attempt attempt(set, slot, b, std::move(names), std::move(change), std::move(reduce), genesis);
  drive(acceptors, attempt, from);
  return attempt;
}

// A transfer to its end, settled at both sets when it commits.
Attempt move(Acceptors& acceptors, const ReplicaSet& from_set, const ReplicaSet& to_set, const identity::NodeId& from, Ballot b) {
  auto attempt = Attempt::transfer(from_set, to_set, slot, b);
  drive(acceptors, attempt, from);
  if (attempt.phase() == Attempt::Phase::committed)
    for (const auto& at : attempt.recipients()) acceptors.settle(at, from, Settle{from_set, to_set, slot, b});
  return attempt;
}

Change put(const std::string& value) {
  return [value](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.size(), bytes(value)), {}}; };
}

Change identity_change() {
  return [](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.begin(), current.end()), {}}; };
}

// SET NX: written only if absent; the result says whether this change won. A retry after
// a commit whose answers were lost reads its own write: it recognises it as its win, as a
// real client tags its writes (the simulator found the ambiguity).
Change set_nx(const std::string& value) {
  return [value](std::span<const std::optional<Bytes>> current) {
    if (current[0] && *current[0] == bytes(value)) return Changed{{current[0]}, bytes("won")};
    if (current[0]) return Changed{{current[0]}, bytes("lost")};
    return Changed{{bytes(value)}, bytes("won")};
  };
}

TEST(Agreement, AChangeCommitsOnAMajorityAndAReadSeesIt) {
  Acceptors acceptors;
  const auto written = run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v"));
  ASSERT_EQ(written.phase(), Attempt::Phase::committed);
  EXPECT_EQ(written.accepted().size(), 3u);
  const auto read = run(acceptors, three, node(2), ballot(2, 2), {bytes("k")}, identity_change());
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  ASSERT_TRUE(read.values()[0]);
  EXPECT_EQ(text(*read.values()[0]), "v");
}

// A fencing token (Louhi's review): the ballot that last altered a value, the same on
// every read until the value changes.
TEST(Agreement, ChangedAtStaysThroughReadsAndMovesWithAChange) {
  Acceptors acceptors;
  const auto written = run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v"));
  ASSERT_EQ(written.phase(), Attempt::Phase::committed);
  EXPECT_EQ(written.changed_at()[0], ballot(1, 1));
  const auto read = run(acceptors, three, node(2), ballot(2, 2), {bytes("k")}, identity_change());
  EXPECT_EQ(read.changed_at()[0], ballot(1, 1));
  const auto again = run(acceptors, three, node(3), ballot(3, 3), {bytes("k")}, identity_change());
  EXPECT_EQ(again.changed_at()[0], ballot(1, 1));
  const auto moved = run(acceptors, three, node(1), ballot(4, 1), {bytes("k")}, put("w"));
  EXPECT_EQ(moved.changed_at()[0], ballot(4, 1));
}

// A ballot below a promise is refused, naming the promise, so the next attempt passes it.
TEST(Agreement, ALowerBallotIsRefusedNamingTheHigher) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(5, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const auto late = run(acceptors, three, node(2), ballot(3, 2), {bytes("k")}, put("w"));
  EXPECT_EQ(late.phase(), Attempt::Phase::refused);
  ASSERT_TRUE(late.higher());
  EXPECT_EQ(*late.higher(), ballot(5, 1));
  const auto passed = run(acceptors, three, node(2), Ballot{late.higher()->counter + 1, node(2), node(102)}, {bytes("k")}, put("w"));
  EXPECT_EQ(passed.phase(), Attempt::Phase::committed);
}

// Several names of one set commit together, or not at all.
TEST(Agreement, SeveralNamesCommitTogether) {
  Acceptors acceptors;
  const auto both = run(acceptors, three, node(1), ballot(1, 1), {bytes("a"), bytes("b")}, put("x"));
  ASSERT_EQ(both.phase(), Attempt::Phase::committed);
  // A promise held on one name refuses the pair.
  ASSERT_EQ(run(acceptors, three, node(2), ballot(9, 2), {bytes("b")}, put("y")).phase(), Attempt::Phase::committed);
  const auto pair = run(acceptors, three, node(3), ballot(5, 3), {bytes("a"), bytes("b")}, put("z"));
  EXPECT_EQ(pair.phase(), Attempt::Phase::refused);
  const auto read = run(acceptors, three, node(1), ballot(10, 1), {bytes("a"), bytes("b")}, identity_change());
  EXPECT_EQ(text(*read.values()[0]), "x");
  EXPECT_EQ(text(*read.values()[1]), "y");
}

// Who may take part (Lugh's review): a sender outside the set, or a set this acceptor is
// not in, is refused.
TEST(Agreement, OnlyTheSetsMembersTakePart) {
  const Prepare request{three, std::nullopt, slot, ballot(1, 9), {bytes("k")}};
  const Held none{{}, {}, {bytes("k")}, {AcceptorState{}}};
  const auto stranger = on_prepare(node(1), node(9), request, none, genesis);
  ASSERT_TRUE(stranger.answer.refused);
  EXPECT_FALSE(stranger.keep);
  const auto outside = on_prepare(node(7), node(1), Prepare{three, std::nullopt, slot, ballot(1, 1), {bytes("k")}}, none, genesis);
  ASSERT_TRUE(outside.answer.refused);
}

// One set at a time (Dagda's review): once a slot is served by a set, a proposer naming
// another is refused at every overlapping acceptor, told which set serves it.
TEST(Agreement, ANameServedByOneSetRefusesAnother) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const auto other = ReplicaSet::of({node(2), node(3), node(4)}, 1);
  const auto refused = run(acceptors, other, node(2), ballot(2, 2), {bytes("k")}, put("w"));
  EXPECT_EQ(refused.phase(), Attempt::Phase::refused);
  ASSERT_TRUE(refused.serving());
  EXPECT_EQ(*refused.serving(), three);
}

// Deleting a name (Dagda's review, safety 2): a tombstone every replica holds is purged,
// and the floor it leaves refuses a stale proposer that would bring the old value back. A
// replica that missed the tombstone keeps its state, so nothing is purged from under it.
TEST(Agreement, AForgottenNameStaysForgotten) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const auto tombstone = run(acceptors, three, node(2), ballot(2, 2), {bytes("k")},
                             [](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.size()), {}}; });
  ASSERT_EQ(tombstone.phase(), Attempt::Phase::committed);
  for (const auto& at : three.nodes) {
    const auto step = acceptors.purge(at, node(2), Purge{three, slot, ballot(2, 2), {bytes("k")}});
    ASSERT_FALSE(step.refused);
    EXPECT_EQ(step.forgotten, 1u);
    EXPECT_FALSE(acceptors.held[at]["k"].value);
    EXPECT_TRUE(acceptors.held[at]["k"].accepted.zero());
    EXPECT_GE(acceptors.floor[at], ballot(2, 2));
  }
  // A proposer that had prepared before the delete, at a ballot below the floor, is refused.
  const auto stale = run(acceptors, three, node(3), ballot(2, 1), {bytes("k")}, put("resurrected"));
  EXPECT_EQ(stale.phase(), Attempt::Phase::refused);
  const auto read = run(acceptors, three, node(1), ballot(9, 1), {bytes("k")}, identity_change());
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_FALSE(read.values()[0]);

  // Only the exact tombstone is purged: a replica holding something else keeps it.
  ASSERT_EQ(run(acceptors, three, node(1), ballot(10, 1), {bytes("j")}, put("w")).phase(), Attempt::Phase::committed);
  const auto kept = acceptors.purge(node(1), node(1), Purge{three, slot, ballot(2, 2), {bytes("j")}});
  EXPECT_EQ(kept.forgotten, 0u);
  EXPECT_EQ(text(*acceptors.held[node(1)]["j"].value), "w");
}

// Value-less prepare (the design's "Value-less prepare and deltas"): an answer says whether
// a value is held and carries the store's summary, and carries the value only when wanted.
// A store that loaded value-less answers the same.
TEST(ValueLess, APrepareWithoutValuesAnswersWhatIsHeldAndItsSummary) {
  const AcceptorState stored{ballot(1, 1), ballot(1, 1), bytes("a megabyte"), ballot(1, 1), bytes("v3"), true};
  const SlotState served{three, std::nullopt, Ballot{}};
  Held held{served, {}, {bytes("k"), bytes("j")}, {stored, AcceptorState{}}};
  Prepare request{three, std::nullopt, slot, ballot(2, 1), {bytes("k"), bytes("j")}, false};
  const auto check = [&](const PrepareStep& step, bool carried) {
    ASSERT_FALSE(step.answer.refused);
    ASSERT_EQ(step.answer.states.size(), 2u);
    const auto& k = step.answer.states[0];
    EXPECT_EQ(k.accepted, ballot(1, 1));
    EXPECT_EQ(k.changed_at, ballot(1, 1));
    EXPECT_TRUE(k.has_value);
    EXPECT_EQ(text(k.summary), "v3");
    EXPECT_EQ(k.value.has_value(), carried);
    if (carried) {
      EXPECT_EQ(text(*k.value), "a megabyte");
    }
    EXPECT_FALSE(step.answer.states[1].has_value);
    EXPECT_FALSE(step.answer.states[1].value);
  };
  check(on_prepare(node(1), node(1), request, held, genesis), false);
  held.states[0].value.reset();
  const auto loaded_without = on_prepare(node(1), node(1), request, held, genesis);
  check(loaded_without, false);
  // What it keeps is still held without its value, for the store to leave as it is.
  ASSERT_TRUE(loaded_without.keep);
  EXPECT_TRUE(loaded_without.keep->states[0].has_value);
  EXPECT_EQ(loaded_without.keep->states[0].promised, ballot(2, 1));
  held.states[0].value = bytes("a megabyte");
  request.values = true;
  check(on_prepare(node(1), node(1), request, held, genesis), true);
}

// An accept carries a whole value: the state holds it, or holds none for a tombstone, and its
// summary is left empty for the store to give it.
TEST(ValueLess, AnAcceptMarksTheValueHeldAndLeavesItsSummaryToTheStore) {
  const AcceptorState stored{ballot(1, 1), ballot(1, 1), std::nullopt, ballot(1, 1), bytes("old"), true};
  const Held held{{three, std::nullopt, Ballot{}}, {}, {bytes("k")}, {stored}};
  const auto written =
      on_accept(node(1), node(1), Accept{three, std::nullopt, slot, ballot(2, 1), {bytes("k")}, {bytes("new")}, {ballot(2, 1)}, {}, {}}, held, genesis);
  ASSERT_TRUE(written.keep);
  EXPECT_TRUE(written.keep->states[0].has_value);
  EXPECT_EQ(text(*written.keep->states[0].value), "new");
  EXPECT_TRUE(written.keep->states[0].summary.empty());
  const auto deleted =
      on_accept(node(1), node(1), Accept{three, std::nullopt, slot, ballot(2, 1), {bytes("k")}, {std::nullopt}, {ballot(2, 1)}, {}, {}}, held, genesis);
  ASSERT_TRUE(deleted.keep);
  EXPECT_FALSE(deleted.keep->states[0].has_value);
  EXPECT_FALSE(deleted.keep->states[0].value);
}

// A purge forgets only a tombstone: a value loaded value-less is still a value.
TEST(ValueLess, APurgeTellsATombstoneFromAValueLoadedWithoutIt) {
  const Purge request{three, slot, ballot(2, 2), {bytes("k")}};
  const SlotState served{three, std::nullopt, Ballot{}};
  const Held value{served, {}, {bytes("k")}, {AcceptorState{ballot(2, 2), ballot(2, 2), std::nullopt, ballot(2, 2), {}, true}}};
  EXPECT_EQ(on_purge(node(1), node(2), request, value).forgotten, 0u);
  const Held tombstone{served, {}, {bytes("k")}, {AcceptorState{ballot(2, 2), ballot(2, 2), std::nullopt, ballot(2, 2), {}, false}}};
  EXPECT_EQ(on_purge(node(1), node(2), request, tombstone).forgotten, 1u);
}

// The summary is the store's, but bounded: one over 64 bytes refuses the prepare, and nothing
// is kept.
TEST(ValueLess, ASummaryOverSixtyFourBytesIsRefused) {
  const AcceptorState stored{ballot(1, 1), ballot(1, 1), std::nullopt, ballot(1, 1), Bytes(max_summary + 1, 7), true};
  const Held held{{three, std::nullopt, Ballot{}}, {}, {bytes("k")}, {stored}};
  const auto step = on_prepare(node(1), node(1), Prepare{three, std::nullopt, slot, ballot(2, 1), {bytes("k")}, false}, held, genesis);
  EXPECT_TRUE(step.answer.refused);
  EXPECT_FALSE(step.keep);
}

// The base's value is fetched, not carried: once a majority has promised without values, the
// attempt names the base's ballot and the promisers holding exactly it, and goes on to its
// accept, built from the fetched value, once that is in.
TEST(ValueLess, ABaseIsFetchedByItsBallotFromThePromisersHoldingIt) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const Change append = [](std::span<const std::optional<Bytes>> current) { return Changed{{bytes(text(current[0].value_or(Bytes{})) + "+")}, {}}; };
  Attempt attempt(three, slot, ballot(2, 2), {bytes("k")}, append, {}, genesis);
  ASSERT_TRUE(attempt.value_less());
  EXPECT_FALSE(attempt.prepare().values);
  for (const auto& at : three.nodes) {
    const auto answer = acceptors.prepare(at, node(2), attempt.prepare());
    for (const auto& state : answer.states) EXPECT_FALSE(state.value);
    EXPECT_FALSE(attempt.on_prepare(at, answer));
  }
  ASSERT_EQ(attempt.phase(), Attempt::Phase::fetching);
  ASSERT_EQ(attempt.wanted().size(), 1u);
  EXPECT_EQ(attempt.wanted()[0].name, 0u);
  EXPECT_FALSE(attempt.wanted()[0].answer);
  EXPECT_FALSE(attempt.wanted()[0].since);
  EXPECT_EQ(attempt.wanted()[0].ballot, ballot(1, 1));
  // The majority that promised first, both holding the base.
  EXPECT_EQ(attempt.wanted()[0].holders, (std::vector<identity::NodeId>{node(1), node(2)}));
  // A fetch not wanted is ignored.
  EXPECT_FALSE(attempt.fetched(attempt.wanted()[0].id + 1, bytes("x")));
  const auto accept = attempt.fetched(attempt.wanted()[0].id, bytes("v"));
  ASSERT_TRUE(accept);
  EXPECT_EQ(attempt.phase(), Attempt::Phase::accepting);
  EXPECT_EQ(text(*accept->values[0]), "v+");
  EXPECT_EQ(accept->changed_at[0], ballot(2, 2));
  for (const auto& at : three.nodes) attempt.on_accept(at, acceptors.accept(at, node(2), *accept));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*acceptors.held[node(3)]["k"].value), "v+");
}

// A promiser behind the base is no holder: only one whose accepted ballot is the base's.
TEST(ValueLess, OnlyAPromiserHoldingTheBasesBallotIsAHolder) {
  Acceptors acceptors;
  Attempt write(three, slot, ballot(1, 1), {bytes("k")}, put("v"), {}, genesis);
  drive(acceptors, write, node(1), {}, {node(1), node(2)});
  ASSERT_EQ(write.phase(), Attempt::Phase::committed);
  Attempt attempt(three, slot, ballot(2, 3), {bytes("k")}, identity_change(), {}, genesis);
  ASSERT_TRUE(attempt.value_less());
  EXPECT_FALSE(attempt.on_prepare(node(3), acceptors.prepare(node(3), node(3), attempt.prepare())));
  EXPECT_FALSE(attempt.on_prepare(node(2), acceptors.prepare(node(2), node(3), attempt.prepare())));
  ASSERT_EQ(attempt.phase(), Attempt::Phase::fetching);
  ASSERT_EQ(attempt.wanted().size(), 1u);
  EXPECT_EQ(attempt.wanted()[0].holders, std::vector<identity::NodeId>{node(2)});
}

// A base holding no value, never written or a tombstone, needs no fetch: the accept comes at
// once.
TEST(ValueLess, ABaseHoldingNoValueNeedsNoFetch) {
  Acceptors acceptors;
  const auto erase = [](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.size()), {}}; };
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("gone")}, put("v")).phase(), Attempt::Phase::committed);
  ASSERT_EQ(run(acceptors, three, node(1), ballot(2, 1), {bytes("gone")}, erase).phase(), Attempt::Phase::committed);
  Attempt attempt(three, slot, ballot(3, 2), {bytes("gone"), bytes("never")}, put("w"), {}, genesis);
  ASSERT_TRUE(attempt.value_less());
  EXPECT_FALSE(attempt.on_prepare(node(1), acceptors.prepare(node(1), node(2), attempt.prepare())));
  const auto accept = attempt.on_prepare(node(2), acceptors.prepare(node(2), node(2), attempt.prepare()));
  ASSERT_TRUE(accept);
  EXPECT_TRUE(attempt.wanted().empty());
  EXPECT_EQ(text(*accept->values[0]), "w");
  EXPECT_EQ(text(*accept->values[1]), "w");
}

// No holder gave the base: the attempt is refused, for another ballot, and a value fetched
// after is ignored.
TEST(ValueLess, NoHolderGivingTheBaseRefusesTheAttempt) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  Attempt attempt(three, slot, ballot(2, 2), {bytes("k")}, put("w"), {}, genesis);
  ASSERT_TRUE(attempt.value_less());
  for (const auto& at : three.nodes) attempt.on_prepare(at, acceptors.prepare(at, node(2), attempt.prepare()));
  ASSERT_EQ(attempt.phase(), Attempt::Phase::fetching);
  const auto id = attempt.wanted()[0].id;
  attempt.unfetched("no holder answered");
  EXPECT_EQ(attempt.phase(), Attempt::Phase::refused);
  EXPECT_EQ(attempt.why(), "no holder answered");
  EXPECT_FALSE(attempt.fetched(id, bytes("v")));
}

// Value-less is asked before the prepare goes: by a change, with a reduction or without, and
// (part 5) by a transfer. An attempt already answered keeps asking for values.
TEST(ValueLess, APreparedAttemptStillAsksForValues) {
  const Reduce reduce = [](const NameState& base, std::span<const NameState>) { return Reduced{base.value, {}}; };
  Attempt reducing(three, slot, ballot(1, 1), {bytes("k")}, put("v"), reduce, genesis);
  EXPECT_TRUE(reducing.value_less());
  EXPECT_FALSE(reducing.prepare().values);
  auto moving = Attempt::transfer(three, ReplicaSet::of({node(1), node(2), node(3), node(4)}, 2), slot, ballot(1, 1));
  EXPECT_TRUE(moving.value_less());
  EXPECT_FALSE(moving.prepare().values);
  Acceptors acceptors;
  Attempt prepared(three, slot, ballot(1, 1), {bytes("k")}, put("v"), {}, genesis);
  prepared.on_prepare(node(1), acceptors.prepare(node(1), node(1), prepared.prepare()));
  EXPECT_FALSE(prepared.value_less());
}

// Mimir's option (a): the reduction decides on summaries. Matching summaries mean nothing to
// fetch beyond the base. One that differs is asked for, past the base's summary (Mimir: the
// merge operations past a frontier), fetched by that answer's ballot, and the reduction,
// called again, folds it in.
TEST(ValueLess, AReductionDecidesOnSummariesAndFetchesOnlyWhatItAsksFor) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  for (const auto& at : three.nodes) acceptors.held[at]["k"].summary = bytes("f1");
  size_t calls = 0;
  const Reduce merge = [&calls](const NameState& base, std::span<const NameState> answers) {
    ++calls;
    Reduced out{base.value, {}};
    for (size_t i = 0; i < answers.size(); ++i) {
      if (answers[i].summary == base.summary) continue;
      if (!answers[i].value) {
        out.need.push_back({i, base.summary});
        continue;
      }
      // Past the frontier: folded in. Whole: taken as it is.
      out.value = answers[i].past ? bytes(text(*out.value) + text(*answers[i].value)) : answers[i].value;
    }
    return out;
  };
  const auto prepared = [&](Attempt& attempt) {
    EXPECT_TRUE(attempt.value_less());
    for (const auto& at : three.nodes) attempt.on_prepare(at, acceptors.prepare(at, node(2), attempt.prepare()));
    EXPECT_EQ(attempt.phase(), Attempt::Phase::fetching);
    EXPECT_EQ(attempt.wanted().size(), 1u);
  };

  // Every summary matches: the base alone, and one call.
  Attempt same(three, slot, ballot(2, 2), {bytes("k")}, identity_change(), merge, genesis);
  prepared(same);
  const auto plain = same.fetched(same.wanted()[0].id, bytes("a"));
  ASSERT_TRUE(plain);
  EXPECT_EQ(text(*plain->values[0]), "a");
  EXPECT_EQ(calls, 1u);

  // A merge write reached node 2 outside agreement: its summary moved, its ballot did not.
  calls = 0;
  acceptors.held[node(2)]["k"].summary = bytes("f2");
  Attempt merged(three, slot, ballot(3, 2), {bytes("k")}, identity_change(), merge, genesis);
  prepared(merged);
  // A base is fetched whole: an answer past a summary it never asked with is ignored.
  EXPECT_FALSE(merged.fetched(merged.wanted()[0].id, bytes("a"), true));
  ASSERT_EQ(merged.wanted().size(), 1u);
  EXPECT_FALSE(merged.wanted()[0].answer);
  EXPECT_FALSE(merged.fetched(merged.wanted()[0].id, bytes("a")));
  ASSERT_EQ(merged.phase(), Attempt::Phase::fetching);
  ASSERT_EQ(merged.wanted().size(), 1u);
  const auto asked = merged.wanted()[0];
  ASSERT_TRUE(asked.answer);
  EXPECT_EQ(asked.ballot, ballot(1, 1));
  ASSERT_TRUE(asked.since);
  EXPECT_EQ(text(*asked.since), "f1");
  // Only the acceptor holding that state: the same ballot is not the same value here.
  EXPECT_EQ(asked.holders, std::vector<identity::NodeId>{node(2)});
  const auto folded = merged.fetched(asked.id, bytes("+b"), true);
  ASSERT_TRUE(folded);
  EXPECT_EQ(text(*folded->values[0]), "a+b");
  EXPECT_EQ(calls, 2u);

  // The holder could not serve past the frontier and answered whole: the reduction is told so.
  Attempt whole(three, slot, ballot(4, 2), {bytes("k")}, identity_change(), merge, genesis);
  prepared(whole);
  EXPECT_FALSE(whole.fetched(whole.wanted()[0].id, bytes("a")));
  ASSERT_EQ(whole.wanted().size(), 1u);
  const auto taken = whole.fetched(whole.wanted()[0].id, bytes("a+b"), false);
  ASSERT_TRUE(taken);
  EXPECT_EQ(text(*taken->values[0]), "a+b");
}

// A reduction that asks again for an answer it was given refuses the attempt, rather than
// fetching for ever.
TEST(ValueLess, AReductionAskingTwiceForOneAnswerRefuses) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  const Reduce greedy = [](const NameState&, std::span<const NameState>) { return Reduced{std::nullopt, {{0, bytes("s")}}}; };
  Attempt attempt(three, slot, ballot(2, 2), {bytes("k")}, identity_change(), greedy, genesis);
  ASSERT_TRUE(attempt.value_less());
  for (const auto& at : three.nodes) attempt.on_prepare(at, acceptors.prepare(at, node(2), attempt.prepare()));
  ASSERT_EQ(attempt.phase(), Attempt::Phase::fetching);
  EXPECT_FALSE(attempt.fetched(attempt.wanted()[0].id, bytes("a")));
  ASSERT_EQ(attempt.phase(), Attempt::Phase::fetching);
  EXPECT_FALSE(attempt.fetched(attempt.wanted()[0].id, bytes("past s")));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::refused);
  EXPECT_FALSE(attempt.why().empty());
}

// A change appending '+', returning the delta as well as the whole value.
Change append_with_delta() {
  return [](std::span<const std::optional<Bytes>> current) {
    auto value = current[0].value_or(Bytes{});
    value.push_back('+');
    return Changed{{value}, {}, {Bytes{'+'}}};
  };
}

// Deltas (the design's "Value-less prepare and deltas", part 4): a delta goes to an acceptor
// that answered holding its base, and (part 5) to one not heard from, which asks whole if it
// lacks the base; one that answered behind is sent the whole value. Every acceptor ends with
// the same value.
TEST(Deltas, ADeltaGoesOnlyWhereItsBaseIsHeld) {
  Acceptors acceptors;
  Attempt write(three, slot, ballot(1, 1), {bytes("k")}, put("a"), {}, genesis);
  drive(acceptors, write, node(1), {}, {node(1), node(2)});
  ASSERT_EQ(write.phase(), Attempt::Phase::committed);
  Attempt attempt(three, slot, ballot(2, 1), {bytes("k")}, append_with_delta(), {}, genesis);
  EXPECT_FALSE(attempt.on_prepare(node(1), acceptors.prepare(node(1), node(1), attempt.prepare())));
  const auto accept = attempt.on_prepare(node(3), acceptors.prepare(node(3), node(1), attempt.prepare()));
  ASSERT_TRUE(accept);
  EXPECT_EQ(text(*accept->values[0]), "a+");
  ASSERT_EQ(accept->deltas.size(), 1u);
  EXPECT_EQ(accept->bases[0], ballot(1, 1));
  const auto to_holder = attempt.accept_for(node(1), *accept);
  ASSERT_EQ(to_holder.deltas.size(), 1u);
  EXPECT_TRUE(to_holder.deltas[0]);
  EXPECT_FALSE(to_holder.values[0]);
  // Answered behind the base: whole.
  const auto whole = attempt.accept_for(node(3), *accept);
  EXPECT_TRUE(whole.deltas.empty());
  EXPECT_EQ(text(*whole.values[0]), "a+");
  // Not heard from: the delta, with the base's ballot and summary.
  const auto unheard = attempt.accept_for(node(2), *accept);
  ASSERT_EQ(unheard.deltas.size(), 1u);
  EXPECT_TRUE(unheard.deltas[0]);
  EXPECT_EQ(unheard.summaries.size(), 1u);
  for (const auto& at : three.nodes) EXPECT_FALSE(attempt.on_accept(at, acceptors.accept(at, node(1), attempt.accept_for(at, *accept))));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::committed);
  for (const auto& at : three.nodes) EXPECT_EQ(text(*acceptors.held[at]["k"].value), "a+");
}

// The rule: a delta whose base is not held here asks for the name whole and keeps nothing; a
// delta on the base is given to the store; an accept already kept at its ballot (resent, or
// replayed after recovery) is kept again without its delta, so it is never applied twice.
TEST(Deltas, TheRuleAsksWholeOffTheBaseAndNeverAppliesADeltaTwice) {
  const SlotState served{three, std::nullopt, Ballot{}};
  const Held at_base{served, {}, {bytes("k")}, {AcceptorState{ballot(1, 1), ballot(1, 1), bytes("a"), ballot(1, 1), {}, true}}};
  Accept accept{three, std::nullopt, slot, ballot(2, 1), {bytes("k")}, {std::nullopt}, {ballot(2, 1)}, {}, {}, {Bytes{'+'}}, {ballot(1, 1)}};
  const auto applied = on_accept(node(1), node(1), accept, at_base, genesis);
  EXPECT_TRUE(applied.answer.need_whole.empty());
  ASSERT_TRUE(applied.keep);
  ASSERT_TRUE(applied.keep->states[0].delta);
  EXPECT_EQ(applied.keep->states[0].base, ballot(1, 1));
  EXPECT_EQ(applied.keep->states[0].accepted, ballot(2, 1));
  EXPECT_FALSE(applied.keep->states[0].value);
  EXPECT_TRUE(applied.keep->states[0].has_value);

  accept.bases[0] = ballot(1, 2);
  const auto off_base = on_accept(node(1), node(1), accept, at_base, genesis);
  EXPECT_EQ(off_base.answer.need_whole, std::vector<size_t>{0});
  EXPECT_FALSE(off_base.answer.refused);
  EXPECT_FALSE(off_base.keep);

  accept.bases[0] = ballot(1, 1);
  const Held kept_already{served, {}, {bytes("k")}, {AcceptorState{ballot(2, 1), ballot(2, 1), bytes("a+"), ballot(2, 1), {}, true}}};
  const auto replayed = on_accept(node(1), node(1), accept, kept_already, genesis);
  EXPECT_TRUE(replayed.answer.need_whole.empty());
  ASSERT_TRUE(replayed.keep);
  EXPECT_FALSE(replayed.keep->states[0].delta);
  EXPECT_EQ(text(*replayed.keep->states[0].value), "a+");

  // The base's ballot with another summary (a merge write outside agreement) is another
  // value: asked for whole.
  Accept merged = accept;
  merged.summaries = {bytes("another state")};
  EXPECT_EQ(on_accept(node(1), node(1), merged, at_base, genesis).answer.need_whole, std::vector<size_t>{0});

  // Both whole and a delta for one name is not an accept Dannan makes.
  accept.values[0] = bytes("a+");
  EXPECT_TRUE(on_accept(node(1), node(1), accept, at_base, genesis).answer.refused);
}

// An acceptor that needs a name whole is sent the accept again with it whole, and then
// counts; one that asks whole again for what it was sent whole counts as refusing.
TEST(Deltas, NeedWholeIsAnsweredWithTheNameWhole) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  Attempt attempt(three, slot, ballot(2, 1), {bytes("k")}, append_with_delta(), {}, genesis);
  std::optional<Accept> accept;
  for (const auto& at : three.nodes)
    if (auto next = attempt.on_prepare(at, acceptors.prepare(at, node(1), attempt.prepare()))) accept = next;
  ASSERT_TRUE(accept);
  const auto again = attempt.on_accept(node(1), AcceptAnswer{std::nullopt, {0}});
  ASSERT_TRUE(again);
  EXPECT_TRUE(again->deltas.empty());
  EXPECT_EQ(text(*again->values[0]), "a+");
  EXPECT_TRUE(attempt.accepted().empty());
  // Whole now, and asked whole again: refused.
  EXPECT_FALSE(attempt.on_accept(node(1), AcceptAnswer{std::nullopt, {0}}));
  EXPECT_EQ(attempt.refused(), std::vector<identity::NodeId>{node(1)});
  // The others take the delta, and commit it.
  for (const auto& at : {node(2), node(3)}) EXPECT_FALSE(attempt.on_accept(at, acceptors.accept(at, node(1), attempt.accept_for(at, *accept))));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::committed);
}

// The ballot only (Kvasir's "already held"): a value the change leaves as it is goes to an
// acceptor holding it as the ballot alone, and that acceptor keeps its value at the new ballot.
TEST(Held, AnUnchangedValueGoesAsTheBallotOnlyToItsHolders) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  Attempt read(three, slot, ballot(2, 2), {bytes("k")}, identity_change(), {}, genesis);
  std::optional<Accept> accept;
  for (const auto& at : three.nodes)
    if (auto next = read.on_prepare(at, acceptors.prepare(at, node(2), read.prepare()))) accept = next;
  ASSERT_TRUE(accept);
  const auto to_holder = read.accept_for(node(1), *accept);
  ASSERT_EQ(to_holder.held.size(), 1u);
  EXPECT_EQ(to_holder.held[0], 1);
  EXPECT_EQ(to_holder.bases[0], ballot(1, 1));
  EXPECT_FALSE(to_holder.values[0]);
  for (const auto& at : three.nodes) read.on_accept(at, acceptors.accept(at, node(2), read.accept_for(at, *accept)));
  EXPECT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*read.values()[0]), "v");
  for (const auto& at : three.nodes) {
    EXPECT_EQ(acceptors.held[at]["k"].accepted, ballot(2, 2));
    EXPECT_EQ(text(*acceptors.held[at]["k"].value), "v");
  }
  // An acceptor that no longer holds the base asks for the name whole.
  const SlotState served{three, std::nullopt, Ballot{}};
  const Held moved_on{served, {}, {bytes("k")}, {AcceptorState{ballot(3, 3), ballot(3, 3), bytes("x"), ballot(3, 3), {}, true}}};
  Accept stale = to_holder;
  stale.ballot = ballot(4, 2);
  EXPECT_EQ(on_accept(node(1), node(2), stale, moved_on, genesis).answer.need_whole, std::vector<size_t>{0});
}

// A transfer goes value-less too: the bases are fetched, an acceptor already holding a name
// (a new one caught up by the store's own means, Kvasir's ranges) is sent its ballot only,
// and one lacking it the whole value.
TEST(Held, ATransferFetchesItsBasesAndSendsTheBallotOnlyToAcceptorsHoldingThem) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("a")}, put("va")).phase(), Attempt::Phase::committed);
  ASSERT_EQ(run(acceptors, three, node(1), ballot(2, 1), {bytes("b")}, put("vb")).phase(), Attempt::Phase::committed);
  // Node 4 caught up with "a" beforehand.
  acceptors.held[node(4)]["a"] = acceptors.held[node(1)]["a"];
  const auto four = ReplicaSet::of({node(1), node(2), node(3), node(4)}, 2);
  auto moving = Attempt::transfer(three, four, slot, ballot(3, 1));
  ASSERT_TRUE(moving.value_less());
  // Node 4 promises among the first, so its answer counts; node 3's is past enough.
  for (const auto& at : {node(4), node(1), node(2), node(3)}) {
    const auto answer = acceptors.prepare(at, node(1), moving.prepare());
    for (const auto& state : answer.states) EXPECT_FALSE(state.value);
    moving.on_prepare(at, answer);
  }
  ASSERT_EQ(moving.phase(), Attempt::Phase::fetching);
  ASSERT_EQ(moving.wanted().size(), 2u);
  std::optional<Accept> accept;
  // A copy: each fetch takes its entry out of wanted().
  const auto wanted_now = moving.wanted();
  for (const auto& wanted : wanted_now)
    if (auto next = moving.fetched(wanted.id, bytes(text(wanted.key) == "a" ? "va" : "vb"))) accept = next;
  ASSERT_TRUE(accept);
  ASSERT_EQ(accept->names.size(), 2u);
  const auto to_new = moving.accept_for(node(4), *accept);
  ASSERT_EQ(to_new.held.size(), 2u);
  EXPECT_EQ(to_new.held[0], 1);  // "a": held already
  EXPECT_EQ(to_new.held[1], 0);  // "b": whole
  EXPECT_EQ(text(*to_new.values[1]), "vb");
  const auto to_old = moving.accept_for(node(1), *accept);
  EXPECT_EQ(to_old.held, (std::vector<uint8_t>{1, 1}));
  for (const auto& at : moving.recipients()) moving.on_accept(at, acceptors.accept(at, node(1), moving.accept_for(at, *accept)));
  EXPECT_EQ(moving.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*acceptors.held[node(4)]["a"].value), "va");
  EXPECT_EQ(text(*acceptors.held[node(4)]["b"].value), "vb");
  EXPECT_EQ(acceptors.held[node(4)]["a"].accepted, ballot(3, 1));
}

// The value-less change (part 6): the change sees each name's base, never its value, and
// returns a delta; no value is fetched or sent, and the acceptors apply the delta in place.
TEST(ValueLessChange, CommitsADeltaWithNoValueAnywhere) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  std::vector<Base> seen;
  const DeltaChange plus = [&seen](std::span<const Base> bases) {
    seen.assign(bases.begin(), bases.end());
    return Deltas{{Bytes{'+'}}, bytes("ok")};
  };
  auto attempt = Attempt::deltas(three, slot, ballot(2, 1), {bytes("k")}, plus, genesis, node(1));
  EXPECT_FALSE(attempt.prepare().values);
  std::optional<Accept> accept;
  for (const auto& at : three.nodes)
    if (auto next = attempt.on_prepare(at, acceptors.prepare(at, node(1), attempt.prepare()))) accept = next;
  ASSERT_TRUE(accept);
  EXPECT_TRUE(attempt.wanted().empty());
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].ballot, ballot(1, 1));
  EXPECT_TRUE(seen[0].has_value);
  EXPECT_TRUE(seen[0].local);
  EXPECT_FALSE(accept->values[0]);
  ASSERT_TRUE(accept->deltas[0]);
  EXPECT_EQ(attempt.accept_for(node(3), *accept).deltas, accept->deltas);
  for (const auto& at : three.nodes) attempt.on_accept(at, acceptors.accept(at, node(1), *accept));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(attempt.result()), "ok");
  for (const auto& at : three.nodes) EXPECT_EQ(text(*acceptors.held[at]["k"].value), "a+");
}

// A value-less read: no delta, and every promiser at the base, so that state is chosen
// already. It commits with no accept, and nothing changes at any acceptor.
TEST(ValueLessChange, AReadWhosePromisersAgreeCommitsWithNoAccept) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  const DeltaChange none = [](std::span<const Base> bases) { return Deltas{std::vector<std::optional<Bytes>>(bases.size()), bytes("read")}; };
  auto read = Attempt::deltas(three, slot, ballot(2, 2), {bytes("k")}, none, genesis, node(2));
  for (const auto& at : three.nodes) EXPECT_FALSE(read.on_prepare(at, acceptors.prepare(at, node(2), read.prepare())));
  EXPECT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_TRUE(read.read_only());
  EXPECT_EQ(read.changed_at()[0], ballot(1, 1));
  for (const auto& at : three.nodes) EXPECT_EQ(acceptors.held[at]["k"].accepted, ballot(1, 1));

  // One promiser behind: not chosen by this majority, so the ballot only goes out.
  Attempt write(three, slot, ballot(3, 1), {bytes("k")}, put("b"), {}, genesis);
  drive(acceptors, write, node(1), {}, {node(1), node(2)});
  auto behind = Attempt::deltas(three, slot, ballot(4, 2), {bytes("k")}, none, genesis, node(2));
  EXPECT_FALSE(behind.on_prepare(node(2), acceptors.prepare(node(2), node(2), behind.prepare())));
  const auto accept = behind.on_prepare(node(3), acceptors.prepare(node(3), node(2), behind.prepare()));
  ASSERT_TRUE(accept);
  EXPECT_FALSE(behind.read_only());
  ASSERT_EQ(accept->held.size(), 1u);
  EXPECT_EQ(accept->held[0], 1);
  EXPECT_EQ(accept->bases[0], ballot(3, 1));
}

// The owner declines (its base not local, or moved under its lock): nothing is sent, and the
// attempt ends, "declined".
TEST(ValueLessChange, ADeclinedChangeEndsAtOnce) {
  Acceptors acceptors;
  const DeltaChange decline = [](std::span<const Base>) { return Deltas{{}, {}, true}; };
  auto attempt = Attempt::deltas(three, slot, ballot(1, 1), {bytes("k")}, decline, genesis, node(1));
  for (const auto& at : three.nodes) EXPECT_FALSE(attempt.on_prepare(at, acceptors.prepare(at, node(1), attempt.prepare())));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::refused);
  EXPECT_TRUE(attempt.declined());
  EXPECT_EQ(attempt.why(), "declined");
  EXPECT_FALSE(attempt.accept());
}

// An acceptor lacking the base needs the name whole, which a value-less change does not carry:
// it is named and not counted, and is sent nothing more.
TEST(ValueLessChange, AnAcceptorNeedingANameWholeIsNamedNotResent) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  const DeltaChange plus = [](std::span<const Base>) { return Deltas{{Bytes{'+'}}, {}}; };
  auto attempt = Attempt::deltas(three, slot, ballot(2, 1), {bytes("k")}, plus, genesis, node(1));
  for (const auto& at : three.nodes) attempt.on_prepare(at, acceptors.prepare(at, node(1), attempt.prepare()));
  ASSERT_EQ(attempt.phase(), Attempt::Phase::accepting);
  EXPECT_FALSE(attempt.on_accept(node(3), AcceptAnswer{std::nullopt, {0}}));
  EXPECT_EQ(attempt.need_whole(), std::vector<identity::NodeId>{node(3)});
  EXPECT_EQ(attempt.refused(), std::vector<identity::NodeId>{node(3)});
  attempt.on_accept(node(1), AcceptAnswer{});
  attempt.on_accept(node(2), AcceptAnswer{});
  EXPECT_EQ(attempt.phase(), Attempt::Phase::committed);
}

// A retry that finds this proposer's own earlier accept newest commits it as it stands, the
// ballot only: the delta is never applied twice.
TEST(ValueLessChange, ARetryAfterAPartialAcceptNeverAppliesItsDeltaTwice) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  size_t calls = 0;
  const DeltaChange plus = [&calls](std::span<const Base>) {
    ++calls;
    return Deltas{{Bytes{'+'}}, bytes("first")};
  };
  auto first = Attempt::deltas(three, slot, ballot(2, 1), {bytes("k")}, plus, genesis, node(1));
  drive(acceptors, first, node(1), {}, {node(1)});
  EXPECT_NE(first.phase(), Attempt::Phase::committed);
  auto retry = Attempt::deltas(three, slot, ballot(3, 1), {bytes("k")}, plus, genesis, node(1));
  retry.own({{ballot(2, 1), bytes("first")}});
  std::optional<Accept> accept;
  for (const auto& at : {node(1), node(2)})
    if (auto next = retry.on_prepare(at, acceptors.prepare(at, node(1), retry.prepare()))) accept = next;
  ASSERT_TRUE(accept);
  EXPECT_EQ(calls, 1u);
  EXPECT_FALSE(accept->deltas[0]);
  EXPECT_EQ(accept->held[0], 1);
  EXPECT_EQ(text(retry.result()), "first");
}

// A minority answers: nothing commits.
TEST(Agreement, AMinorityCommitsNothing) {
  Acceptors acceptors;
  Attempt attempt(three, slot, ballot(1, 1), {bytes("k")}, put("v"), {}, genesis);
  EXPECT_FALSE(attempt.on_prepare(node(1), acceptors.prepare(node(1), node(1), attempt.prepare())));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::preparing);
  // A repeat from the same replica does not make a majority.
  EXPECT_FALSE(attempt.on_prepare(node(1), acceptors.prepare(node(1), node(1), attempt.prepare())));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::preparing);
}

// The gap found in building reconfiguration (the design's "Reconfiguration: the gap ... and
// the fix"). A transfer whose accept reached one node of each set did not commit. A proposer
// that takes the new set from that node's hint must not commit on nodes that never heard of
// the slot, nor go on past the unfinished transfer; the old set meanwhile commits, and the
// transfer, once finished, carries what it committed.
TEST(Reconfiguration, AnUnfinishedTransferIsNeitherTakenFromItsHintNorLost) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const auto next = ReplicaSet::of({node(4), node(5), node(6)}, 2);
  auto partial = Attempt::transfer(three, next, slot, ballot(2, 4));
  drive(acceptors, partial, node(4), {node(1), node(2), node(4), node(5)}, {node(1), node(4)});
  EXPECT_EQ(partial.phase(), Attempt::Phase::accepting);
  ASSERT_EQ(acceptors.slots[node(4)].served, next);
  ASSERT_EQ(acceptors.slots[node(4)].pending_from, three);

  // On the hint, a majority of the new set that never heard of the slot: refused.
  Attempt hinted(next, slot, ballot(3, 5), {bytes("k")}, put("lost"), {}, genesis);
  drive(acceptors, hinted, node(5), {node(5), node(6)});
  EXPECT_EQ(hinted.phase(), Attempt::Phase::refused);
  EXPECT_FALSE(hinted.unfinished());
  // A majority that includes the pending node: refused, naming the transfer to finish.
  Attempt pending(next, slot, ballot(4, 5), {bytes("k")}, put("lost"), {}, genesis);
  drive(acceptors, pending, node(5), {node(4), node(5)});
  EXPECT_EQ(pending.phase(), Attempt::Phase::refused);
  ASSERT_TRUE(pending.unfinished());
  EXPECT_EQ(*pending.unfinished(), three);

  // The old set still commits on the majority the transfer did not move.
  Attempt old_side(three, slot, ballot(5, 3), {bytes("k")}, put("w"), {}, genesis);
  drive(acceptors, old_side, node(3), {node(2), node(3)}, {node(2), node(3)});
  ASSERT_EQ(old_side.phase(), Attempt::Phase::committed);

  // Finished, the transfer carries the old set's last commit, and the old set is refused.
  const auto finished = move(acceptors, three, next, node(5), ballot(6, 5));
  ASSERT_EQ(finished.phase(), Attempt::Phase::committed);
  const auto read = run(acceptors, next, node(6), ballot(7, 6), {bytes("k")}, identity_change());
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*read.values()[0]), "w");
  const auto stale = run(acceptors, three, node(2), ballot(8, 2), {bytes("k")}, put("stale"));
  EXPECT_EQ(stale.phase(), Attempt::Phase::refused);
  ASSERT_TRUE(stale.serving());
  EXPECT_EQ(*stale.serving(), next);
}

// After a committed transfer, a proposer naming the old set is refused and told the new
// one; a name never written in the slot is found on the new set, not the old; and every
// name the old set held is carried.
TEST(Reconfiguration, ATransferMovesEveryNameAndRefusesTheOldSet) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("a"), bytes("b")}, put("x")).phase(), Attempt::Phase::committed);
  const auto next = ReplicaSet::of({node(2), node(3), node(4)}, 2);
  const auto moved = move(acceptors, three, next, node(2), ballot(2, 2));
  ASSERT_EQ(moved.phase(), Attempt::Phase::committed);
  EXPECT_EQ(moved.names(), (std::vector<Name>{bytes("a"), bytes("b")}));
  for (const auto& at : next.nodes) {
    EXPECT_EQ(acceptors.slots[at].served, next);
    EXPECT_FALSE(acceptors.slots[at].pending_from);
  }
  const auto fresh_old = run(acceptors, three, node(1), ballot(3, 1), {bytes("fresh")}, put("old"));
  EXPECT_EQ(fresh_old.phase(), Attempt::Phase::refused);
  EXPECT_EQ(fresh_old.serving(), next);
  ASSERT_EQ(run(acceptors, next, node(4), ballot(4, 4), {bytes("fresh")}, put("new")).phase(), Attempt::Phase::committed);
  const auto read = run(acceptors, next, node(4), ballot(5, 4), {bytes("a"), bytes("b"), bytes("fresh")}, identity_change());
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*read.values()[0]), "x");
  EXPECT_EQ(text(*read.values()[1]), "x");
  EXPECT_EQ(text(*read.values()[2]), "new");
}

// A plain change that commits between a transfer's prepare and its accept refuses the
// accept, so the transfer cannot carry an older value past it; a retry carries the new one.
TEST(Reconfiguration, AChangeRacingATransferIsCarriedOrRefusesIt) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const auto next = ReplicaSet::of({node(3), node(4), node(5)}, 2);
  auto racing = Attempt::transfer(three, next, slot, ballot(2, 4));
  std::optional<Accept> accept;
  for (const auto& at : racing.recipients())
    if (auto made = racing.on_prepare(at, acceptors.prepare(at, node(4), racing.prepare()))) accept = made;
  ASSERT_TRUE(accept);
  // A plain change above the transfer's ballot commits on the old set.
  ASSERT_EQ(run(acceptors, three, node(2), ballot(3, 2), {bytes("k")}, put("w")).phase(), Attempt::Phase::committed);
  for (const auto& at : racing.recipients()) racing.on_accept(at, acceptors.accept(at, node(4), *accept));
  EXPECT_EQ(racing.phase(), Attempt::Phase::refused);
  ASSERT_TRUE(racing.higher());
  const auto again = move(acceptors, three, next, node(4), Ballot{racing.higher()->counter + 1, node(4), node(104)});
  ASSERT_EQ(again.phase(), Attempt::Phase::committed);
  const auto read = run(acceptors, next, node(5), ballot(9, 5), {bytes("k")}, identity_change());
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*read.values()[0]), "w");
}

// A slot no acceptor has recorded is served by the genesis set alone: a plain change on any
// other set is refused, and only the genesis set moves it.
TEST(Reconfiguration, AnUnrecordedSlotBelongsToTheGenesisSet) {
  Acceptors acceptors;
  const auto other = ReplicaSet::of({node(4), node(5), node(6)}, 1);
  EXPECT_EQ(run(acceptors, other, node(4), ballot(1, 4), {bytes("k")}, put("v")).phase(), Attempt::Phase::refused);
  const auto stolen = move(acceptors, other, ReplicaSet::of({node(4), node(5), node(6)}, 2), node(4), ballot(2, 4));
  EXPECT_EQ(stolen.phase(), Attempt::Phase::refused);
  // Founding: a change with no names, on the genesis set, records the slot as settled.
  const auto founded = run(acceptors, genesis, node(1), ballot(3, 1), {}, identity_change());
  ASSERT_EQ(founded.phase(), Attempt::Phase::committed);
  for (const auto& at : genesis.nodes) {
    EXPECT_EQ(acceptors.slots[at].served, genesis);
    EXPECT_FALSE(acceptors.slots[at].pending_from);
  }
}

// One-node genesis grown to five (the maintainer's ruling): each step a transfer, the value intact.
TEST(Reconfiguration, AOneNodeGenesisGrowsToFive) {
  Acceptors acceptors;
  // This test's own genesis: one node.
  const auto one = ReplicaSet::of({node(1)}, 0);
  acceptors.founders = one;
  Attempt write(one, slot, ballot(1, 1), {bytes("k")}, put("v"), {}, one);
  ASSERT_EQ(drive(acceptors, write, node(1)).phase(), Attempt::Phase::committed);
  ReplicaSet current = one;
  for (uint8_t size = 2; size <= 5; ++size) {
    std::vector<identity::NodeId> grown;
    for (uint8_t n = 1; n <= size; ++n) grown.push_back(node(n));
    const auto next = ReplicaSet::of(grown, size - 1);
    ASSERT_EQ(move(acceptors, current, next, node(size), ballot(10 * size, size)).phase(), Attempt::Phase::committed) << size;
    current = next;
  }
  const auto read = run(acceptors, current, node(5), ballot(100, 5), {bytes("k")}, identity_change());
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*read.values()[0]), "v");
}

// An acceptor of the old set that missed the transfer's accept learns the move by settle,
// and refuses the old set from then on.
TEST(Reconfiguration, SettleReachesAnAcceptorThatMissedTheAccept) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const auto next = ReplicaSet::of({node(1), node(2), node(4)}, 2);
  auto moving = Attempt::transfer(three, next, slot, ballot(2, 4));
  drive(acceptors, moving, node(4), {}, {node(1), node(2), node(4)});
  ASSERT_EQ(moving.phase(), Attempt::Phase::committed);
  EXPECT_EQ(acceptors.slots[node(3)].served, three);
  const Settle settle{three, next, slot, ballot(2, 4)};
  for (const auto& at : moving.recipients()) EXPECT_FALSE(acceptors.settle(at, node(4), settle).refused);
  EXPECT_EQ(acceptors.slots[node(3)].served, next);
  for (const auto& at : next.nodes) EXPECT_FALSE(acceptors.slots[at].pending_from);
  // Settled twice: nothing changes.
  const auto before = acceptors.slots;
  for (const auto& at : moving.recipients()) acceptors.settle(at, node(4), settle);
  EXPECT_EQ(acceptors.slots, before);
}

// A member of both sets that missed a transfer's accept and its settle still holds the old
// set: it does not stop a change on the new set (the simulator's seed 13 found it did), and
// that change's accept brings it up to date.
TEST(Reconfiguration, ALaggardOfTheOldSetNeitherBlocksTheNewSetNorStaysBehind) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const auto next = ReplicaSet::of({node(1), node(2), node(3), node(4), node(5)}, 2);
  auto moving = Attempt::transfer(three, next, slot, ballot(2, 4));
  drive(acceptors, moving, node(4), {}, {node(1), node(2), node(4), node(5)});
  ASSERT_EQ(moving.phase(), Attempt::Phase::committed);
  for (const auto& at : {node(1), node(2), node(4)}) acceptors.settle(at, node(4), Settle{three, next, slot, ballot(2, 4)});
  ASSERT_EQ(acceptors.slots[node(3)].served, three);
  // Node 3 answers first, holding the old set: counted as a refusal at most, never as proof
  // the new set is wrong.
  Attempt write(next, slot, ballot(3, 3), {bytes("k")}, put("w"), {}, genesis);
  drive(acceptors, write, node(3), {node(3), node(1), node(2), node(4), node(5)});
  ASSERT_EQ(write.phase(), Attempt::Phase::committed) << write.why();
  EXPECT_EQ(acceptors.slots[node(3)].served, next);
  // A proposer still naming the old set is refused all the same.
  const auto stale = run(acceptors, three, node(1), ballot(4, 1), {bytes("k")}, put("stale"));
  EXPECT_EQ(stale.phase(), Attempt::Phase::refused);
  EXPECT_EQ(stale.serving(), next);
}

// A retry after a partial accept (the simulator's seed 30, with the fast path): the retry's
// majority finds the proposer's own earlier accept newest, so the change is already in it;
// it commits it unchanged, with that attempt's result, rather than applying the change twice.
TEST(Agreement, ARetryAfterAPartialAcceptAppliesTheChangeOnce) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v")).phase(), Attempt::Phase::committed);
  const Change append = [](std::span<const std::optional<Bytes>> current) {
    auto next = *current[0];
    next.push_back('+');
    return Changed{{next}, next};
  };
  Attempt partial(three, slot, ballot(2, 1), {bytes("k")}, append, {}, genesis);
  drive(acceptors, partial, node(1), {}, {node(1)});
  ASSERT_EQ(partial.phase(), Attempt::Phase::accepting);
  Attempt retry(three, slot, ballot(3, 1), {bytes("k")}, append, {}, genesis);
  retry.own({{ballot(2, 1), partial.result()}});
  drive(acceptors, retry, node(1), {node(1), node(2)});
  ASSERT_EQ(retry.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*retry.values()[0]), "v+");
  EXPECT_EQ(text(retry.result()), "v+");
}

// The fast path (the design's "Fast path"): the proposer that committed last changes the
// same names again in one round, its accept carrying its prepare.
// The accept an attempt makes, sent to `to` with a reservation of the proposer's next ballot.
void accept_reserving(Acceptors& acceptors, Attempt& attempt, const identity::NodeId& from, Ballot reserve, std::vector<identity::NodeId> to = {}) {
  auto accept = *attempt.accept();
  accept.reserve = reserve;
  if (to.empty()) to = attempt.recipients();
  for (const auto& at : to) attempt.on_accept(at, acceptors.accept(at, from, accept));
}

// A plain change to its end, its accept reserving `reserve`.
Attempt run_reserving(Acceptors& acceptors, const ReplicaSet& set, const identity::NodeId& from, Ballot b, std::vector<Name> names, Change change,
                      Ballot reserve) {
  Attempt attempt(set, slot, b, std::move(names), std::move(change), {}, genesis);
  for (const auto& at : set.nodes) (void)attempt.on_prepare(at, acceptors.prepare(at, from, attempt.prepare()));
  accept_reserving(acceptors, attempt, from, reserve);
  return attempt;
}

TEST(FastPath, TheLastCommitterChangesInOneRound) {
  Acceptors acceptors;
  const auto first = run_reserving(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v"), ballot(2, 1));
  ASSERT_EQ(first.phase(), Attempt::Phase::committed);
  auto fast = Attempt::fast(three, slot, ballot(2, 1), {bytes("k")}, put("w"), ballot(1, 1), first.values(), first.changed_at());
  ASSERT_TRUE(fast.is_fast());
  EXPECT_EQ(fast.phase(), Attempt::Phase::accepting);
  accept_reserving(acceptors, fast, node(1), ballot(3, 1));
  ASSERT_EQ(fast.phase(), Attempt::Phase::committed);
  // Unaltered, a fast identity change keeps the fencing token.
  auto again = Attempt::fast(three, slot, ballot(3, 1), {bytes("k")}, identity_change(), ballot(2, 1), fast.values(), fast.changed_at());
  accept_reserving(acceptors, again, node(1), ballot(4, 1));
  ASSERT_EQ(again.phase(), Attempt::Phase::committed);
  EXPECT_EQ(again.changed_at()[0], ballot(2, 1));
  const auto read = run(acceptors, three, node(2), ballot(5, 2), {bytes("k")}, identity_change());
  EXPECT_EQ(text(*read.values()[0]), "w");
}

// Anything prepared in between, by another proposer, refuses the fast path at a majority,
// so it can never commit over a value it has not seen.
TEST(FastPath, AnythingPreparedInBetweenRefusesIt) {
  Acceptors acceptors;
  const auto first = run_reserving(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v"), ballot(2, 1));
  ASSERT_EQ(first.phase(), Attempt::Phase::committed);
  // Another proposer must pass the reservation, and does.
  EXPECT_EQ(run(acceptors, three, node(2), ballot(1, 2), {bytes("k")}, put("early")).phase(), Attempt::Phase::refused);
  ASSERT_EQ(run(acceptors, three, node(2), ballot(3, 2), {bytes("k")}, put("theirs")).phase(), Attempt::Phase::committed);
  auto fast = Attempt::fast(three, slot, ballot(2, 1), {bytes("k")}, put("mine"), ballot(1, 1), first.values(), first.changed_at());
  accept_reserving(acceptors, fast, node(1), ballot(4, 1));
  EXPECT_EQ(fast.phase(), Attempt::Phase::refused);
  ASSERT_TRUE(fast.higher());
  EXPECT_EQ(*fast.higher(), ballot(3, 2));
  // Even a prepare that committed nothing is enough to send it the full way.
  const auto read = run_reserving(acceptors, three, node(1), ballot(5, 1), {bytes("k")}, identity_change(), ballot(6, 1));
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  auto stale = Attempt::fast(three, slot, ballot(6, 1), {bytes("k")}, put("late"), ballot(5, 1), read.values(), read.changed_at());
  (void)acceptors.prepare(node(2), node(3), Prepare{three, std::nullopt, slot, ballot(7, 3), {bytes("k")}});
  (void)acceptors.prepare(node(3), node(3), Prepare{three, std::nullopt, slot, ballot(7, 3), {bytes("k")}});
  accept_reserving(acceptors, stale, node(1), ballot(8, 1));
  EXPECT_EQ(stale.phase(), Attempt::Phase::refused);
}

// What the simulator found (seed 30) before reservations: a fast accept taken by a minority
// while another proposer committed elsewhere must never outrank that commit. Reserved at the
// last commit, the fast ballot sits below anything that won since.
TEST(FastPath, AFastValueAtAMinorityNeverOutranksACommitInBetween) {
  Acceptors acceptors;
  const auto first = run_reserving(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v"), ballot(2, 1));
  ASSERT_EQ(first.phase(), Attempt::Phase::committed);
  Attempt theirs(three, slot, ballot(3, 2), {bytes("k")}, put("theirs"), {}, genesis);
  drive(acceptors, theirs, node(2), {node(2), node(3)}, {node(2), node(3)});
  ASSERT_EQ(theirs.phase(), Attempt::Phase::committed);
  auto fast = Attempt::fast(three, slot, ballot(2, 1), {bytes("k")}, put("mine"), ballot(1, 1), first.values(), first.changed_at());
  accept_reserving(acceptors, fast, node(1), ballot(4, 1), {node(1)});
  ASSERT_EQ(acceptors.held[node(1)]["k"].accepted, ballot(2, 1));
  const auto read = run(acceptors, three, node(3), ballot(9, 3), {bytes("k")}, identity_change());
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*read.values()[0]), "theirs");
}

// The fast path across a transfer (the design's contract): the last committer's cached
// ballot is for the old set, so its next accept is refused, told the new set, and it never
// commits on the old set again.
TEST(FastPath, IsLostAcrossATransfer) {
  Acceptors acceptors;
  const auto first = run_reserving(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("v"), ballot(2, 1));
  ASSERT_EQ(first.phase(), Attempt::Phase::committed);
  const auto next = ReplicaSet::of({node(2), node(3), node(4)}, 2);
  ASSERT_EQ(move(acceptors, three, next, node(2), ballot(3, 2)).phase(), Attempt::Phase::committed);
  auto fast = Attempt::fast(three, slot, ballot(2, 1), {bytes("k")}, put("old set"), ballot(1, 1), first.values(), first.changed_at());
  accept_reserving(acceptors, fast, node(1), ballot(4, 1));
  EXPECT_EQ(fast.phase(), Attempt::Phase::refused);
  EXPECT_EQ(fast.serving(), next);
  const auto read = run(acceptors, next, node(4), ballot(9, 4), {bytes("k")}, identity_change());
  EXPECT_EQ(text(*read.values()[0]), "v");
}

// The default slot function: fixed, in range, and refusing a table it cannot have.
TEST(Reconfiguration, SlotOfIsFixedAndInRange) {
  EXPECT_EQ(slot_of(bytes("user:1"), 256), slot_of(bytes("user:1"), 256));
  for (uint32_t count : {1u, 7u, 256u, 8192u}) EXPECT_LT(slot_of(bytes("anything"), count), count);
  // FNV-1a 64 of nothing is its offset basis.
  EXPECT_EQ(slot_of({}, 8192), static_cast<Slot>(0xcbf29ce484222325ull % 8192));
  EXPECT_THROW((void)slot_of(bytes("k"), 0), std::invalid_argument);
  EXPECT_THROW((void)slot_of(bytes("k"), max_slots + 1), std::invalid_argument);
}

// The owner's reduction (Mimir's review): given the highest-ballot base and every answer,
// it may advance the base by what other answers carry.
TEST(Agreement, TheOwnersReductionAdvancesTheBase) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  // A merge write that reached one replica of the majority outside agreement.
  acceptors.held[node(2)]["k"].value = bytes("a+b");
  const Reduce join = [](const NameState& base, std::span<const NameState> answers) {
    auto out = base.value;
    for (const auto& each : answers)
      if (each.value && out && each.value->size() > out->size()) out = each.value;
    return Reduced{out, {}};
  };
  const auto read = run(acceptors, three, node(2), ballot(2, 2), {bytes("k")}, identity_change(), join);
  ASSERT_EQ(read.phase(), Attempt::Phase::committed);
  EXPECT_EQ(text(*read.values()[0]), "a+b");
}

// Two proposers race SET NX on one name over three acceptors. Every message is delivered
// in a seeded order, some dropped; a refused or stalled attempt retries above the highest
// ballot it heard of. Whatever the order, exactly one proposer wins, the other sees the
// winner's value, and the register holds it.
TEST(Agreement, RacingProposersNeverBothWin) {
  for (uint32_t seed = 1; seed <= 3000; ++seed) {
    std::mt19937 random(seed);
    Acceptors acceptors;
    struct Proposer {
      identity::NodeId id;
      std::string value;
      uint64_t counter = 0;
      uint64_t generation = 0;
      std::unique_ptr<Attempt> attempt;
      std::optional<std::string> result;
      std::optional<std::string> saw;
    };
    std::vector<Proposer> proposers(2);
    proposers[0].id = node(1);
    proposers[0].value = "one";
    proposers[1].id = node(2);
    proposers[1].value = "two";
    struct Event {
      size_t proposer;
      uint64_t generation;
      identity::NodeId acceptor;
      int kind;  // 0 prepare, 1 prepare answer, 2 accept, 3 accept answer
      // A late prepare is its own attempt's, not the proposer's latest.
      std::optional<Prepare> request;
      std::optional<PrepareAnswer> promised;
      std::optional<Accept> accept;
      std::optional<AcceptAnswer> accepted;
    };
    std::vector<Event> queue;
    const auto start = [&](size_t i, uint64_t past) {
      auto& p = proposers[i];
      p.counter = std::max(p.counter, past) + 1;
      ++p.generation;
      p.attempt = std::make_unique<Attempt>(three, slot, Ballot{p.counter, p.id, node(static_cast<uint8_t>(100 + i))}, std::vector<Name>{bytes("lock")},
                                            set_nx(p.value), Reduce{}, genesis);
      for (const auto& at : three.nodes) queue.push_back({i, p.generation, at, 0, p.attempt->prepare(), {}, {}, {}});
    };
    start(0, 0);
    start(1, 0);
    for (int steps = 0; steps < 20000 && (!proposers[0].result || !proposers[1].result); ++steps) {
      // A proposer with nothing in flight and no answer retries.
      for (size_t i = 0; i < proposers.size(); ++i) {
        auto& p = proposers[i];
        if (p.result) continue;
        const bool waiting = std::any_of(queue.begin(), queue.end(), [&](const Event& e) { return e.proposer == i && e.generation == p.generation; });
        if (!waiting) start(i, p.attempt->higher() ? p.attempt->higher()->counter : 0);
      }
      if (queue.empty()) continue;
      const size_t pick = std::uniform_int_distribution<size_t>(0, queue.size() - 1)(random);
      Event event = std::move(queue[pick]);
      queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(pick));
      if (std::uniform_int_distribution<int>(0, 9)(random) == 0) continue;  // dropped
      auto& p = proposers[event.proposer];
      const bool current = event.generation == p.generation && !p.result;
      switch (event.kind) {
        case 0:
          queue.push_back({event.proposer, event.generation, event.acceptor, 1, {}, acceptors.prepare(event.acceptor, p.id, *event.request), {}, {}});
          break;
        case 1:
          if (!current) break;
          if (auto accept = p.attempt->on_prepare(event.acceptor, *event.promised))
            for (const auto& at : three.nodes) queue.push_back({event.proposer, event.generation, at, 2, {}, {}, accept, {}});
          if (p.attempt->phase() == Attempt::Phase::refused) start(event.proposer, p.attempt->higher() ? p.attempt->higher()->counter : 0);
          break;
        case 2:
          queue.push_back({event.proposer, event.generation, event.acceptor, 3, {}, {}, {}, acceptors.accept(event.acceptor, p.id, *event.accept)});
          break;
        case 3:
          if (!current) break;
          p.attempt->on_accept(event.acceptor, *event.accepted);
          if (p.attempt->phase() == Attempt::Phase::committed) {
            p.result = text(p.attempt->result());
            p.saw = text(*p.attempt->values()[0]);
          } else if (p.attempt->phase() == Attempt::Phase::refused) {
            start(event.proposer, p.attempt->higher() ? p.attempt->higher()->counter : 0);
          }
          break;
      }
    }
    ASSERT_TRUE(proposers[0].result && proposers[1].result) << "seed " << seed << ": a proposer never finished";
    const int wins = (*proposers[0].result == "won") + (*proposers[1].result == "won");
    ASSERT_EQ(wins, 1) << "seed " << seed;
    const auto& winner = *proposers[0].result == "won" ? proposers[0] : proposers[1];
    const auto& loser = *proposers[0].result == "won" ? proposers[1] : proposers[0];
    EXPECT_EQ(*loser.saw, winner.value) << "seed " << seed;
    Acceptors& final_state = acceptors;
    const auto read = run(final_state, three, node(3), ballot(1000000, 3), {bytes("lock")}, identity_change());
    ASSERT_EQ(read.phase(), Attempt::Phase::committed) << "seed " << seed;
    EXPECT_EQ(text(*read.values()[0]), winner.value) << "seed " << seed;
  }
}

// The wire: every message round-trips, a name may hold any byte, and a decoder refuses
// what is out of bounds or out of order rather than guessing.
TEST(AgreementWire, EveryMessageRoundTrips) {
  const Name binary{'k', 0, 0xff};
  const PrepareMessage prepare{"mimir", {three, std::nullopt, slot, ballot(4, 1), {binary, bytes("b")}}};
  const auto prepared = decode_prepare(encode(prepare));
  EXPECT_EQ(prepared.space, "mimir");
  EXPECT_EQ(prepared.prepare.set, three);
  EXPECT_FALSE(prepared.prepare.to);
  EXPECT_EQ(prepared.prepare.slot, slot);
  EXPECT_EQ(prepared.prepare.ballot, ballot(4, 1));
  EXPECT_EQ(prepared.prepare.names, prepare.prepare.names);
  EXPECT_TRUE(prepared.prepare.values);
  EXPECT_FALSE(decode_prepare(encode(PrepareMessage{"mimir", {three, std::nullopt, slot, ballot(4, 1), {binary}, false}})).prepare.values);

  const auto four = ReplicaSet::of({node(1), node(2), node(3), node(4)}, 2);
  const auto moving = decode_prepare(encode(PrepareMessage{"mimir", {three, four, slot, ballot(5, 1), {}}}));
  ASSERT_TRUE(moving.prepare.to);
  EXPECT_EQ(*moving.prepare.to, four);

  const AcceptMessage accept{"lugh/totp", {three, four, slot, ballot(4, 1), {binary}, {std::nullopt}, {ballot(2, 2)}, std::nullopt, std::nullopt}};
  const auto accepted = decode_accept(encode(accept));
  EXPECT_EQ(accepted.accept.names, accept.accept.names);
  EXPECT_EQ(accepted.accept.to, four);
  EXPECT_FALSE(accepted.accept.fast_from);
  const auto fast = decode_accept(
      encode(AcceptMessage{"mimir", {three, std::nullopt, slot, ballot(5, 1), {binary}, {bytes("w")}, {ballot(5, 1)}, ballot(4, 1), ballot(6, 1)}}));
  EXPECT_EQ(fast.accept.fast_from, ballot(4, 1));
  EXPECT_EQ(fast.accept.reserve, ballot(6, 1));
  EXPECT_FALSE(accepted.accept.values[0]);
  EXPECT_EQ(accepted.accept.changed_at[0], ballot(2, 2));

  const PrepareAnswer promise{std::nullopt,
                              four,
                              three,
                              {binary, bytes("b"), bytes("c")},
                              {{ballot(1, 1), bytes("v"), ballot(1, 1), bytes("sum"), true},
                               {Ballot{}, std::nullopt, Ballot{}, {}, false},
                               {ballot(2, 1), std::nullopt, ballot(2, 1), Bytes(max_summary, 9), true}}};
  const auto promised = decode_prepare_answer(encode(promise));
  ASSERT_EQ(promised.states.size(), 3u);
  EXPECT_EQ(promised.served, four);
  EXPECT_EQ(promised.pending_from, three);
  EXPECT_EQ(promised.names, promise.names);
  EXPECT_EQ(text(*promised.states[0].value), "v");
  EXPECT_EQ(text(promised.states[0].summary), "sum");
  EXPECT_TRUE(promised.states[0].has_value);
  EXPECT_FALSE(promised.states[1].value);
  EXPECT_FALSE(promised.states[1].has_value);
  // Held, not carried: a value-less prepare's answer.
  EXPECT_FALSE(promised.states[2].value);
  EXPECT_TRUE(promised.states[2].has_value);
  EXPECT_EQ(promised.states[2].summary, Bytes(max_summary, 9));

  const auto delta = decode_accept(encode(AcceptMessage{"mimir",
                                                        {three,
                                                         std::nullopt,
                                                         slot,
                                                         ballot(5, 1),
                                                         {binary, bytes("b")},
                                                         {std::nullopt, bytes("w")},
                                                         {ballot(5, 1), ballot(5, 1)},
                                                         {},
                                                         {},
                                                         {Bytes{'+'}, std::nullopt},
                                                         {ballot(4, 1), Ballot{}}}}));
  ASSERT_EQ(delta.accept.deltas.size(), 2u);
  EXPECT_EQ(*delta.accept.deltas[0], Bytes{'+'});
  EXPECT_EQ(delta.accept.bases[0], ballot(4, 1));
  EXPECT_FALSE(delta.accept.deltas[1]);
  EXPECT_EQ(text(*delta.accept.values[1]), "w");
  EXPECT_TRUE(accepted.accept.deltas.empty());
  Accept held_only{three, std::nullopt, slot, ballot(5, 1), {binary}, {std::nullopt}, {ballot(5, 1)}, {}, {}, {}, {ballot(4, 1)}, {1}};
  const auto ballot_only = decode_accept(encode(AcceptMessage{"mimir", held_only}));
  ASSERT_EQ(ballot_only.accept.held.size(), 1u);
  EXPECT_EQ(ballot_only.accept.held[0], 1);
  EXPECT_EQ(ballot_only.accept.bases[0], ballot(4, 1));
  EXPECT_FALSE(ballot_only.accept.deltas[0]);
  EXPECT_FALSE(ballot_only.accept.values[0]);
  const auto four_fetch = decode_fetch(encode(FetchMessage{"mimir", three, slot, binary, ballot(3, 2), 0, 0, std::nullopt, four}));
  EXPECT_EQ(four_fetch.to, four);
  EXPECT_EQ(decode_accept_answer(encode(AcceptAnswer{std::nullopt, {0, 3}})).need_whole, (std::vector<size_t>{0, 3}));

  const AcceptAnswer refusal{Refusal{"the slot is served by another replica set", ballot(9, 3), four, three}};
  const auto refused = decode_accept_answer(encode(refusal));
  ASSERT_TRUE(refused.refused);
  EXPECT_EQ(*refused.refused->higher, ballot(9, 3));
  EXPECT_EQ(*refused.refused->serving, four);
  EXPECT_EQ(*refused.refused->pending_from, three);

  // The sender's slot table, fixed by the count and the genesis set.
  const auto table = table_fingerprint(256, three);
  EXPECT_NE(table, table_fingerprint(512, three));
  EXPECT_NE(table, table_fingerprint(256, four));
  EXPECT_EQ(decode_prepare(encode(PrepareMessage{"mimir", {three, std::nullopt, slot, ballot(4, 1), {binary}}, table})).table, table);

  const auto settled = decode_settle(encode(SettleMessage{"mimir", {three, four, slot, ballot(6, 2)}}));
  EXPECT_EQ(settled.settle.from, three);
  EXPECT_EQ(settled.settle.to, four);
  EXPECT_EQ(settled.settle.slot, slot);
  EXPECT_EQ(settled.settle.ballot, ballot(6, 2));
  const auto purged = decode_purge(encode(PurgeMessage{"mimir", {three, slot, ballot(6, 2), {binary}}}));
  EXPECT_EQ(purged.purge.names, std::vector<Name>{binary});

  const auto since = decode_fetch(encode(FetchMessage{"mimir", three, slot, binary, ballot(3, 2), 0, table, bytes("frontier")}));
  ASSERT_TRUE(since.since);
  EXPECT_EQ(text(*since.since), "frontier");
  const auto fetch = decode_fetch(encode(FetchMessage{"mimir", three, slot, binary, ballot(3, 2), 1024, table}));
  EXPECT_FALSE(fetch.since);
  EXPECT_EQ(fetch.space, "mimir");
  EXPECT_EQ(fetch.set, three);
  EXPECT_EQ(fetch.slot, slot);
  EXPECT_EQ(fetch.name, binary);
  EXPECT_EQ(fetch.ballot, ballot(3, 2));
  EXPECT_EQ(fetch.offset, 1024u);
  EXPECT_EQ(fetch.table, table);
  const auto piece = decode_fetch_answer(encode(FetchAnswer{std::nullopt, true, 3000, 1000, Bytes(1000, 7)}));
  EXPECT_FALSE(piece.past);
  EXPECT_TRUE(decode_fetch_answer(encode(FetchAnswer{std::nullopt, true, 10, 0, Bytes(10, 1), true})).past);
  EXPECT_TRUE(piece.held);
  EXPECT_EQ(piece.total, 3000u);
  EXPECT_EQ(piece.offset, 1000u);
  EXPECT_EQ(piece.piece, Bytes(1000, 7));
  const auto moved_on = decode_fetch_answer(encode(FetchAnswer{}));
  EXPECT_FALSE(moved_on.held);
  EXPECT_FALSE(moved_on.refused);
}

TEST(AgreementWire, RefusesWhatItCannotTrust) {
  auto bytes_of = encode(PrepareMessage{"mimir", {three, std::nullopt, slot, ballot(4, 1), {bytes("k")}}});
  bytes_of[0] = 2;  // a version it does not speak
  EXPECT_THROW(decode_prepare(bytes_of), net::DecodeError);
  auto truncated = encode(PrepareMessage{"mimir", {three, std::nullopt, slot, ballot(4, 1), {bytes("k")}}});
  truncated.pop_back();
  EXPECT_THROW(decode_prepare(truncated), net::DecodeError);
  // A set whose nodes are out of order is not a set Dannan makes.
  ReplicaSet unsorted{{node(3), node(1)}, 1};
  EXPECT_THROW(decode_prepare(encode(PrepareMessage{"mimir", {unsorted, std::nullopt, slot, ballot(4, 1), {bytes("k")}}})), net::DecodeError);
  // A transfer's prepare naming registers, a slot past the largest table, or trailing bytes.
  EXPECT_THROW(decode_prepare(encode(PrepareMessage{"mimir", {three, three, slot, ballot(4, 1), {bytes("k")}}})), net::DecodeError);
  EXPECT_THROW(decode_prepare(encode(PrepareMessage{"mimir", {three, std::nullopt, max_slots, ballot(4, 1), {bytes("k")}}})), net::DecodeError);
  auto trailing = encode(AcceptAnswer{});
  trailing.push_back(0);
  EXPECT_THROW(decode_accept_answer(trailing), net::DecodeError);
  // A prepare's values flag other than 0 or 1.
  auto flag = encode(PrepareMessage{"mimir", {three, std::nullopt, slot, ballot(4, 1), {bytes("k")}}});
  flag.back() = 2;
  EXPECT_THROW(decode_prepare(flag), net::DecodeError);
  // An answer's has-value bit (its last byte) other than 0 or 1, or a value carried for a name
  // that holds none.
  const PrepareAnswer carried{std::nullopt, {}, {}, {}, {{ballot(1, 1), bytes("v"), ballot(1, 1), {}, true}}};
  auto held = encode(carried);
  held.back() = 2;
  EXPECT_THROW(decode_prepare_answer(held), net::DecodeError);
  held.back() = 0;
  EXPECT_THROW(decode_prepare_answer(held), net::DecodeError);
  // A summary over 64 bytes is neither sent nor taken.
  const PrepareAnswer long_summary{std::nullopt, {}, {}, {}, {{ballot(1, 1), std::nullopt, ballot(1, 1), Bytes(max_summary + 1, 1), true}}};
  EXPECT_THROW(encode(long_summary), std::invalid_argument);
  // A fetched piece outside its value, a piece with "not held", or a piece over 1 MiB.
  EXPECT_THROW(decode_fetch_answer(encode(FetchAnswer{std::nullopt, true, 100, 50, Bytes(51, 1)})), net::DecodeError);
  EXPECT_THROW(decode_fetch_answer(encode(FetchAnswer{std::nullopt, true, 100, 101, {}})), net::DecodeError);
  EXPECT_THROW(decode_fetch_answer(encode(FetchAnswer{std::nullopt, false, 100, 0, Bytes(1, 1)})), net::DecodeError);
  EXPECT_THROW(decode_fetch_answer(encode(FetchAnswer{std::nullopt, false, 0, 0, {}, true})), net::DecodeError);
  EXPECT_THROW(encode(FetchAnswer{std::nullopt, true, max_piece + 1, 0, Bytes(max_piece + 1, 1)}), std::invalid_argument);
  // A refusal that also asks for names whole, or a name both whole and a delta.
  EXPECT_THROW(decode_accept_answer(encode(AcceptAnswer{Refusal{"no", std::nullopt, std::nullopt, std::nullopt}, {0}})), net::DecodeError);
  EXPECT_THROW(decode_accept(encode(AcceptMessage{
                   "mimir", {three, std::nullopt, slot, ballot(5, 1), {bytes("k")}, {bytes("w")}, {ballot(5, 1)}, {}, {}, {Bytes{'+'}}, {ballot(4, 1)}}})),
               net::DecodeError);
  // A since over 64 bytes is not a summary.
  EXPECT_THROW(encode(FetchMessage{"mimir", three, slot, bytes("k"), ballot(1, 1), 0, 0, Bytes(max_summary + 1, 1)}), std::invalid_argument);
  // A fetch offset past the largest value.
  EXPECT_THROW(decode_fetch(encode(FetchMessage{"mimir", three, slot, bytes("k"), ballot(1, 1), max_value + 1, 0})), net::DecodeError);
}

// The fencing token's bytes order as the ballots do.
TEST(AgreementWire, ABallotsBytesOrderAsItDoes) {
  EXPECT_LT(encode_ballot(ballot(1, 9)), encode_ballot(ballot(2, 1)));
  EXPECT_LT(encode_ballot(ballot(2, 1)), encode_ballot(ballot(2, 2)));
  EXPECT_EQ(encode_ballot(ballot(5, 5)), encode_ballot(ballot(5, 5)));
}

}  // namespace
}  // namespace dannan::agree
