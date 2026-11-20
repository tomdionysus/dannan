//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The production in-memory acceptor store (TODO/2026-11-20-stack-plan.md, item 1; Mimir's,
// Kvasir's and Louhi's conditions): agreement.h's contract text, completions always posted,
// keep() all or nothing, a delta it cannot apply asked for whole, and not ready after a
// restart until the hosted system says it has caught up.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <optional>
#include <string>
#include <vector>

#include "dannan/agree/memory_store.h"
#include "dannan/identity/crypto.h"

namespace dannan::agree {
namespace {

Name name(std::string_view text) { return Name(text.begin(), text.end()); }
Bytes bytes(std::string_view text) { return Bytes(text.begin(), text.end()); }

Ballot ballot(uint64_t counter) {
  Ballot b;
  b.counter = counter;
  b.node.bytes[0] = 1;
  return b;
}

// A state carrying a whole value, accepted at `at`.
AcceptorState whole(uint64_t at, std::string_view value) {
  AcceptorState state;
  state.promised = ballot(at);
  state.accepted = ballot(at);
  state.changed_at = ballot(at);
  state.value = bytes(value);
  state.has_value = true;
  return state;
}

// A state carrying a delta on the value held at `base`.
AcceptorState delta(uint64_t at, uint64_t base, std::string_view more) {
  AcceptorState state;
  state.promised = ballot(at);
  state.accepted = ballot(at);
  state.changed_at = ballot(at);
  state.has_value = true;
  state.delta = bytes(more);
  state.base = ballot(base);
  return state;
}

Held held_of(std::vector<std::pair<Name, AcceptorState>> entries, Ballot floor = {}) {
  Held held;
  held.floor = floor;
  held.slot.served = ReplicaSet::of({identity::NodeId{}}, 0);
  for (auto& [n, s] : entries) {
    held.names.push_back(n);
    held.states.push_back(s);
  }
  return held;
}

// Appends the delta to the value: the test's owner form.
MemoryStore::Codec appending() {
  MemoryStore::Codec codec;
  codec.apply = [](const Bytes& value, const Bytes& more) -> std::optional<Bytes> {
    if (more == bytes("!")) return std::nullopt;
    Bytes out = value;
    out.insert(out.end(), more.begin(), more.end());
    return out;
  };
  return codec;
}

class MemoryStoreTest : public ::testing::Test {
 protected:
  Held load(MemoryStore& store, Slot slot, std::vector<Name> names, bool every = false, bool values = true) {
    std::optional<Held> out;
    store.load(slot, names, every, values, [&](Held h) { out = std::move(h); });
    EXPECT_FALSE(out) << "load answered inline";
    io.run();
    io.restart();
    EXPECT_TRUE(out);
    return out ? *out : Held{};
  }
  AcceptorStore::Kept keep(MemoryStore& store, Slot slot, const Held& held) {
    std::optional<AcceptorStore::Kept> out;
    store.keep(slot, held, [&](AcceptorStore::Kept k) { out = std::move(k); });
    EXPECT_FALSE(out) << "keep answered inline";
    io.run();
    io.restart();
    EXPECT_TRUE(out);
    return out ? *out : AcceptorStore::Kept{};
  }

  boost::asio::io_context io;
};

TEST_F(MemoryStoreTest, ANewStoreIsReadyOnlyWhenTold) {
  MemoryStore restarted(io.get_executor(), false);
  EXPECT_FALSE(restarted.ready());
  restarted.set_ready();
  EXPECT_TRUE(restarted.ready());
  MemoryStore founding(io.get_executor(), true);
  EXPECT_TRUE(founding.ready());
}

TEST_F(MemoryStoreTest, ANameNeverSeenLoadsAsADefaultState) {
  MemoryStore store(io.get_executor(), true);
  const auto held = load(store, 3, {name("k")});
  ASSERT_EQ(held.states.size(), 1u);
  EXPECT_EQ(held.states[0], AcceptorState{});
  EXPECT_FALSE(held.slot.served);
  EXPECT_TRUE(held.floor.zero());
}

TEST_F(MemoryStoreTest, AKeptValueIsSummarisedAndLoadedBack) {
  MemoryStore store(io.get_executor(), true);
  ASSERT_TRUE(keep(store, 1, held_of({{name("k"), whole(5, "v1")}}, ballot(2))).kept);
  const auto held = load(store, 1, {name("k")});
  ASSERT_EQ(held.states.size(), 1u);
  EXPECT_EQ(held.states[0].value, bytes("v1"));
  EXPECT_TRUE(held.states[0].has_value);
  const auto digest = identity::sha256(bytes("v1"));
  EXPECT_EQ(held.states[0].summary, Bytes(digest.bytes.begin(), digest.bytes.end()));
  EXPECT_EQ(held.floor, ballot(2));
  EXPECT_TRUE(held.slot.served);
}

TEST_F(MemoryStoreTest, AValuelessLoadSaysWhatIsHeldWithoutTheValue) {
  MemoryStore store(io.get_executor(), true);
  keep(store, 1, held_of({{name("k"), whole(5, "v1")}}));
  const auto held = load(store, 1, {name("k")}, false, false);
  ASSERT_EQ(held.states.size(), 1u);
  EXPECT_FALSE(held.states[0].value);
  EXPECT_TRUE(held.states[0].has_value);
  EXPECT_FALSE(held.states[0].summary.empty());
}

TEST_F(MemoryStoreTest, AStateHeldButNotCarriedKeepsTheValueItHas) {
  MemoryStore store(io.get_executor(), true);
  keep(store, 1, held_of({{name("k"), whole(5, "v1")}}));
  auto promise_only = whole(6, "");
  promise_only.value.reset();
  ASSERT_TRUE(keep(store, 1, held_of({{name("k"), promise_only}})).kept);
  const auto state = store.state(1, name("k"));
  ASSERT_TRUE(state);
  EXPECT_EQ(state->value, bytes("v1"));
  EXPECT_EQ(state->promised, ballot(6));
}

TEST_F(MemoryStoreTest, AStateHeldButNotCarriedWithNothingBeforeKeepsNoValue) {
  MemoryStore store(io.get_executor(), true);
  auto promise_only = whole(6, "");
  promise_only.value.reset();
  ASSERT_TRUE(keep(store, 1, held_of({{name("k"), promise_only}})).kept);
  const auto state = store.state(1, name("k"));
  ASSERT_TRUE(state);
  EXPECT_FALSE(state->value);
  EXPECT_TRUE(state->summary.empty());
}

TEST_F(MemoryStoreTest, AStateWithNoValueKeepsItsBallotsAndNoSummary) {
  MemoryStore store(io.get_executor(), true);
  AcceptorState promise;
  promise.promised = ballot(9);
  ASSERT_TRUE(keep(store, 1, held_of({{name("k"), promise}})).kept);
  const auto state = store.state(1, name("k"));
  ASSERT_TRUE(state);
  EXPECT_EQ(state->promised, ballot(9));
  EXPECT_FALSE(state->has_value);
  EXPECT_TRUE(state->summary.empty());
}

TEST_F(MemoryStoreTest, LoadingEveryNameOfASlotNeverHeldGivesOnlyTheAsked) {
  MemoryStore store(io.get_executor(), true);
  const auto held = load(store, 4, {name("a")}, true);
  EXPECT_EQ(held.names.size(), 1u);
}

TEST_F(MemoryStoreTest, ADeltaOnANameNeverHeldAppliesToAnEmptyValueAtTheZeroBase) {
  MemoryStore store(io.get_executor(), true, appending());
  auto first = delta(6, 0, "ab");
  first.base = {};
  ASSERT_TRUE(keep(store, 1, held_of({{name("a"), first}})).kept);
  EXPECT_EQ(store.state(1, name("a"))->value, bytes("ab"));
}

TEST_F(MemoryStoreTest, ADefaultStateDropsTheName) {
  MemoryStore store(io.get_executor(), true);
  keep(store, 1, held_of({{name("a"), whole(5, "x")}, {name("b"), whole(5, "y")}}));
  EXPECT_EQ(store.size(), 2u);
  keep(store, 1, held_of({{name("a"), AcceptorState{}}}));
  EXPECT_EQ(store.size(), 1u);
  EXPECT_FALSE(store.state(1, name("a")));
}

TEST_F(MemoryStoreTest, WithoutAnApplyEveryDeltaIsAskedForWholeAndNothingIsKept) {
  MemoryStore store(io.get_executor(), true);
  keep(store, 1, held_of({{name("a"), whole(5, "x")}, {name("b"), whole(5, "y")}}));
  const auto kept = keep(store, 1, held_of({{name("a"), whole(6, "x2")}, {name("b"), delta(6, 5, "+")}}));
  EXPECT_FALSE(kept.kept);
  EXPECT_EQ(kept.need_whole, std::vector<size_t>{1});
  EXPECT_EQ(store.state(1, name("a"))->value, bytes("x")) << "part of a refused keep was kept";
}

TEST_F(MemoryStoreTest, ADeltaIsAppliedToTheValueAtItsBase) {
  MemoryStore store(io.get_executor(), true, appending());
  keep(store, 1, held_of({{name("a"), whole(5, "x")}}));
  ASSERT_TRUE(keep(store, 1, held_of({{name("a"), delta(6, 5, "yz")}})).kept);
  const auto state = store.state(1, name("a"));
  EXPECT_EQ(state->value, bytes("xyz"));
  EXPECT_FALSE(state->delta);
  EXPECT_TRUE(state->base.zero());
  const auto digest = identity::sha256(bytes("xyz"));
  EXPECT_EQ(state->summary, Bytes(digest.bytes.begin(), digest.bytes.end()));
}

TEST_F(MemoryStoreTest, ADeltaOnAnotherBaseIsAskedForWhole) {
  MemoryStore store(io.get_executor(), true, appending());
  keep(store, 1, held_of({{name("a"), whole(5, "x")}}));
  const auto kept = keep(store, 1, held_of({{name("a"), delta(6, 4, "y")}}));
  EXPECT_FALSE(kept.kept);
  EXPECT_EQ(kept.need_whole, std::vector<size_t>{0});
}

TEST_F(MemoryStoreTest, ADeltaTheOwnerCannotApplyIsAskedForWholeAndNothingIsKept) {
  MemoryStore store(io.get_executor(), true, appending());
  keep(store, 1, held_of({{name("a"), whole(5, "x")}, {name("b"), whole(5, "y")}}));
  const auto kept = keep(store, 1, held_of({{name("a"), delta(6, 5, "+")}, {name("b"), delta(6, 5, "!")}}));
  EXPECT_FALSE(kept.kept);
  EXPECT_EQ(kept.need_whole, std::vector<size_t>{1});
  EXPECT_EQ(store.state(1, name("a"))->value, bytes("x"));
}

TEST_F(MemoryStoreTest, ASummaryOverTheBoundIsRefused) {
  MemoryStore::Codec codec;
  codec.summary = [](const Bytes&) { return Bytes(max_summary + 1, 0); };
  MemoryStore store(io.get_executor(), true, codec);
  const auto kept = keep(store, 1, held_of({{name("a"), whole(5, "x")}}));
  EXPECT_FALSE(kept.kept);
  EXPECT_TRUE(kept.need_whole.empty());
  EXPECT_FALSE(store.state(1, name("a")));
}

TEST_F(MemoryStoreTest, LoadingEveryNameGivesTheAskedFirstThenTheRestOfTheSlot) {
  MemoryStore store(io.get_executor(), true);
  keep(store, 1, held_of({{name("a"), whole(5, "x")}, {name("b"), whole(5, "y")}, {name("c"), whole(5, "z")}}));
  keep(store, 2, held_of({{name("other"), whole(5, "w")}}));
  const auto held = load(store, 1, {name("b")}, true);
  ASSERT_EQ(held.names.size(), 3u);
  EXPECT_EQ(held.names[0], name("b"));
  EXPECT_EQ(held.states.size(), 3u);
  for (const auto& n : held.names) EXPECT_NE(n, name("other"));
}

TEST_F(MemoryStoreTest, RecordsListEverySlotHeld) {
  MemoryStore store(io.get_executor(), true);
  keep(store, 1, held_of({{name("a"), whole(5, "x")}}));
  keep(store, 7, held_of({{name("b"), whole(5, "y")}}));
  std::optional<std::vector<std::pair<Slot, SlotState>>> out;
  store.records([&](auto records) { out = std::move(records); });
  EXPECT_FALSE(out) << "records answered inline";
  io.run();
  ASSERT_TRUE(out);
  ASSERT_EQ(out->size(), 2u);
  EXPECT_EQ((*out)[0].first, 1u);
  EXPECT_EQ((*out)[1].first, 7u);
}

TEST_F(MemoryStoreTest, AValueIsFetchedOnlyAtItsExactBallot) {
  MemoryStore store(io.get_executor(), true);
  keep(store, 1, held_of({{name("a"), whole(5, "x")}}));
  std::optional<std::optional<AcceptorStore::Fetched>> at, other;
  store.value(1, name("a"), ballot(5), std::nullopt, [&](auto f) { at = f; });
  store.value(1, name("a"), ballot(4), std::nullopt, [&](auto f) { other = f; });
  io.run();
  ASSERT_TRUE(at && *at);
  EXPECT_EQ((*at)->bytes, bytes("x"));
  EXPECT_FALSE((*at)->past);
  ASSERT_TRUE(other);
  EXPECT_FALSE(*other);
}

TEST_F(MemoryStoreTest, AStateNeverHeldIsNone) {
  MemoryStore store(io.get_executor(), true);
  EXPECT_FALSE(store.state(1, name("a")));
  EXPECT_EQ(store.size(), 0u);
}

// each_held (0.10.1, Mimir's ask, so it can drop its own store): the names a slot holds a value
// for, with their states, in the order of the names' bytes, from past `after` if given, until
// the visitor says stop. A paging catch-up resumes after the last name it sent.
std::vector<std::string> walk(const MemoryStore& store, Slot slot, std::optional<Name> after = {}, size_t most = 1000) {
  std::vector<std::string> out;
  store.each_held(slot, after, [&](const Name& n, const AcceptorState& state) {
    EXPECT_TRUE(state.has_value) << "a name with no value was visited";
    out.emplace_back(n.begin(), n.end());
    return out.size() < most;
  });
  return out;
}

TEST_F(MemoryStoreTest, EachHeldVisitsTheNamesHoldingAValueInTheirBytesOrder) {
  MemoryStore store(io.get_executor(), true);
  AcceptorState promised_only;
  promised_only.promised = ballot(1);
  ASSERT_TRUE(keep(store, 4, held_of({{name("b"), whole(1, "2")}, {name("a"), whole(1, "1")}, {name("p"), promised_only}})).kept);
  // A name past every ASCII one, by its bytes: 0xff sorts after 'z'.
  ASSERT_TRUE(keep(store, 4, held_of({{Name{0xff}, whole(2, "3")}, {name("z"), whole(2, "4")}})).kept);
  ASSERT_TRUE(keep(store, 5, held_of({{name("other"), whole(1, "5")}})).kept);
  EXPECT_EQ(walk(store, 4), (std::vector<std::string>{"a", "b", "z", "\xff"}));
  EXPECT_EQ(walk(store, 9), std::vector<std::string>{}) << "a slot never kept holds nothing";
  std::optional<AcceptorState> seen;
  store.each_held(4, std::nullopt, [&](const Name& n, const AcceptorState& state) {
    if (n == name("b")) seen = state;
    return true;
  });
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->value, bytes("2"));
}

TEST_F(MemoryStoreTest, EachHeldStopsWhenTheVisitorSaysSo) {
  MemoryStore store(io.get_executor(), true);
  ASSERT_TRUE(keep(store, 1, held_of({{name("a"), whole(1, "1")}, {name("b"), whole(1, "2")}, {name("c"), whole(1, "3")}})).kept);
  EXPECT_EQ(walk(store, 1, std::nullopt, 2), (std::vector<std::string>{"a", "b"}));
}

TEST_F(MemoryStoreTest, EachHeldResumesStrictlyPastTheNameGivenHeldOrNot) {
  MemoryStore store(io.get_executor(), true);
  ASSERT_TRUE(keep(store, 1, held_of({{name("a"), whole(1, "1")}, {name("c"), whole(1, "3")}, {name("e"), whole(1, "5")}})).kept);
  EXPECT_EQ(walk(store, 1, name("a")), (std::vector<std::string>{"c", "e"}));
  EXPECT_EQ(walk(store, 1, name("b")), (std::vector<std::string>{"c", "e"})) << "from a name not held";
  EXPECT_EQ(walk(store, 1, name("e")), std::vector<std::string>{});
}

// Pages by the last name sent, not by index: a name removed between two pages shifts no later
// name out of the walk (Mimir's reason for `after`).
TEST_F(MemoryStoreTest, APagedWalkSkipsNoNameWhenOneIsRemovedBetweenPages) {
  MemoryStore store(io.get_executor(), true);
  ASSERT_TRUE(keep(store, 1, held_of({{name("a"), whole(1, "1")}, {name("b"), whole(1, "2")}, {name("c"), whole(1, "3")}, {name("d"), whole(1, "4")}})).kept);
  const auto first = walk(store, 1, std::nullopt, 2);
  ASSERT_EQ(first, (std::vector<std::string>{"a", "b"}));
  // "a" is removed: a default state is dropped.
  ASSERT_TRUE(keep(store, 1, held_of({{name("a"), AcceptorState{}}})).kept);
  EXPECT_EQ(walk(store, 1, name(first.back()), 2), (std::vector<std::string>{"c", "d"}));
}

}  // namespace
}  // namespace dannan::agree
