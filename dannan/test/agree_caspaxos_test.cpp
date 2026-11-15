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
  void keep(const identity::NodeId& at, const Held& kept) {
    slots[at] = kept.slot;
    floor[at] = kept.floor;
    for (size_t i = 0; i < kept.names.size(); ++i) held[at][key(kept.names[i])] = kept.states[i];
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
    return out;
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

  const auto four = ReplicaSet::of({node(1), node(2), node(3), node(4)}, 2);
  const auto moving = decode_prepare(encode(PrepareMessage{"mimir", {three, four, slot, ballot(5, 1), {}}}));
  ASSERT_TRUE(moving.prepare.to);
  EXPECT_EQ(*moving.prepare.to, four);

  const AcceptMessage accept{"lugh/totp", {three, four, slot, ballot(4, 1), {binary}, {std::nullopt}, {ballot(2, 2)}}};
  const auto accepted = decode_accept(encode(accept));
  EXPECT_EQ(accepted.accept.names, accept.accept.names);
  EXPECT_EQ(accepted.accept.to, four);
  EXPECT_FALSE(accepted.accept.values[0]);
  EXPECT_EQ(accepted.accept.changed_at[0], ballot(2, 2));

  const PrepareAnswer promise{std::nullopt, four, three, {binary, bytes("b")}, {{ballot(1, 1), bytes("v"), ballot(1, 1)}, {Ballot{}, std::nullopt, Ballot{}}}};
  const auto promised = decode_prepare_answer(encode(promise));
  ASSERT_EQ(promised.states.size(), 2u);
  EXPECT_EQ(promised.served, four);
  EXPECT_EQ(promised.pending_from, three);
  EXPECT_EQ(promised.names, promise.names);
  EXPECT_EQ(text(*promised.states[0].value), "v");
  EXPECT_FALSE(promised.states[1].value);

  const AcceptAnswer refusal{Refusal{"the slot is served by another replica set", ballot(9, 3), four, three}};
  const auto refused = decode_accept_answer(encode(refusal));
  ASSERT_TRUE(refused.refused);
  EXPECT_EQ(*refused.refused->higher, ballot(9, 3));
  EXPECT_EQ(*refused.refused->serving, four);
  EXPECT_EQ(*refused.refused->pending_from, three);

  const auto settled = decode_settle(encode(SettleMessage{"mimir", {three, four, slot, ballot(6, 2)}}));
  EXPECT_EQ(settled.settle.from, three);
  EXPECT_EQ(settled.settle.to, four);
  EXPECT_EQ(settled.settle.slot, slot);
  EXPECT_EQ(settled.settle.ballot, ballot(6, 2));
  const auto purged = decode_purge(encode(PurgeMessage{"mimir", {three, slot, ballot(6, 2), {binary}}}));
  EXPECT_EQ(purged.purge.names, std::vector<Name>{binary});
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
}

// The fencing token's bytes order as the ballots do.
TEST(AgreementWire, ABallotsBytesOrderAsItDoes) {
  EXPECT_LT(encode_ballot(ballot(1, 9)), encode_ballot(ballot(2, 1)));
  EXPECT_LT(encode_ballot(ballot(2, 1)), encode_ballot(ballot(2, 2)));
  EXPECT_EQ(encode_ballot(ballot(5, 5)), encode_ballot(ballot(5, 5)));
}

}  // namespace
}  // namespace dannan::agree
