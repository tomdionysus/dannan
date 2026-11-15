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

// The acceptors' state, kept as a hosted system would, and the rules applied to it.
struct Acceptors {
  std::map<identity::NodeId, std::map<Name, AcceptorState>> held;

  std::vector<AcceptorState> load(const identity::NodeId& at, const std::vector<Name>& names) {
    std::vector<AcceptorState> out;
    for (const auto& name : names) out.push_back(held[at][name]);
    return out;
  }
  void keep(const identity::NodeId& at, const std::vector<Name>& names, const std::vector<AcceptorState>& states) {
    for (size_t i = 0; i < names.size(); ++i) held[at][names[i]] = states[i];
  }
  PrepareAnswer prepare(const identity::NodeId& at, const identity::NodeId& from, const Prepare& request) {
    const auto state = load(at, request.names);
    auto step = on_prepare(at, from, request, state);
    if (step.keep) keep(at, request.names, *step.keep);
    return step.answer;
  }
  AcceptAnswer accept(const identity::NodeId& at, const identity::NodeId& from, const Accept& request) {
    const auto state = load(at, request.names);
    auto step = on_accept(at, from, request, state);
    if (step.keep) keep(at, request.names, *step.keep);
    return step.answer;
  }
};

// Runs one attempt to its end with every replica answering in order.
Attempt run(Acceptors& acceptors, const ReplicaSet& set, const identity::NodeId& from, Ballot b, std::vector<Name> names, Change change, Reduce reduce = {}) {
  Attempt attempt(set, b, std::move(names), std::move(change), std::move(reduce));
  std::optional<Accept> accept;
  for (const auto& at : set.nodes) {
    if (auto next = attempt.on_prepare(at, acceptors.prepare(at, from, attempt.prepare()))) accept = next;
  }
  if (accept)
    for (const auto& at : set.nodes) attempt.on_accept(at, acceptors.accept(at, from, *accept));
  return attempt;
}

Change put(const std::string& value) {
  return [value](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.size(), bytes(value)), {}}; };
}

Change identity_change() {
  return [](std::span<const std::optional<Bytes>> current) { return Changed{std::vector<std::optional<Bytes>>(current.begin(), current.end()), {}}; };
}

// SET NX: written only if absent; the result says whether this change won.
Change set_nx(const std::string& value) {
  return [value](std::span<const std::optional<Bytes>> current) {
    if (current[0]) return Changed{{current[0]}, bytes("lost")};
    return Changed{{bytes(value)}, bytes("won")};
  };
}

const ReplicaSet three = ReplicaSet::of({node(1), node(2), node(3)}, 1);

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
  const Prepare request{three, ballot(1, 9), {bytes("k")}};
  const AcceptorState none;
  const auto stranger = on_prepare(node(1), node(9), request, std::span(&none, 1));
  ASSERT_TRUE(stranger.answer.refused);
  EXPECT_FALSE(stranger.keep);
  const auto outside = on_prepare(node(7), node(1), Prepare{three, ballot(1, 1), {bytes("k")}}, std::span(&none, 1));
  ASSERT_TRUE(outside.answer.refused);
}

// One set at a time (Dagda's review): once a name is served by a set, a proposer naming
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

// A minority answers: nothing commits.
TEST(Agreement, AMinorityCommitsNothing) {
  Acceptors acceptors;
  Attempt attempt(three, ballot(1, 1), {bytes("k")}, put("v"));
  EXPECT_FALSE(attempt.on_prepare(node(1), acceptors.prepare(node(1), node(1), attempt.prepare())));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::preparing);
  // A repeat from the same replica does not make a majority.
  EXPECT_FALSE(attempt.on_prepare(node(1), acceptors.prepare(node(1), node(1), attempt.prepare())));
  EXPECT_EQ(attempt.phase(), Attempt::Phase::preparing);
}

// The owner's reduction (Mimir's review): given the highest-ballot base and every answer,
// it may advance the base by what other answers carry.
TEST(Agreement, TheOwnersReductionAdvancesTheBase) {
  Acceptors acceptors;
  ASSERT_EQ(run(acceptors, three, node(1), ballot(1, 1), {bytes("k")}, put("a")).phase(), Attempt::Phase::committed);
  // A merge write that reached one replica outside agreement.
  acceptors.held[node(3)][bytes("k")].value = bytes("a+b");
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
      p.attempt =
          std::make_unique<Attempt>(three, Ballot{p.counter, p.id, node(static_cast<uint8_t>(100 + i))}, std::vector<Name>{bytes("lock")}, set_nx(p.value));
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
  const PrepareMessage prepare{"mimir", {three, ballot(4, 1), {binary, bytes("b")}}};
  const auto prepared = decode_prepare(encode(prepare));
  EXPECT_EQ(prepared.space, "mimir");
  EXPECT_EQ(prepared.prepare.set, three);
  EXPECT_EQ(prepared.prepare.ballot, ballot(4, 1));
  EXPECT_EQ(prepared.prepare.names, prepare.prepare.names);

  const AcceptMessage accept{"lugh/totp", {three, ballot(4, 1), {binary}, {std::nullopt}, {ballot(2, 2)}}};
  const auto accepted = decode_accept(encode(accept));
  EXPECT_EQ(accepted.accept.names, accept.accept.names);
  EXPECT_FALSE(accepted.accept.values[0]);
  EXPECT_EQ(accepted.accept.changed_at[0], ballot(2, 2));

  const PrepareAnswer promise{{}, {{ballot(1, 1), bytes("v"), ballot(1, 1)}, {Ballot{}, std::nullopt, Ballot{}}}};
  const auto promised = decode_prepare_answer(encode(promise));
  ASSERT_EQ(promised.states.size(), 2u);
  EXPECT_EQ(text(*promised.states[0].value), "v");
  EXPECT_FALSE(promised.states[1].value);

  const AcceptAnswer refusal{Refusal{"a name is served by another replica set", ballot(9, 3), three}};
  const auto refused = decode_accept_answer(encode(refusal));
  ASSERT_TRUE(refused.refused);
  EXPECT_EQ(*refused.refused->higher, ballot(9, 3));
  EXPECT_EQ(*refused.refused->serving, three);
}

TEST(AgreementWire, RefusesWhatItCannotTrust) {
  auto bytes_of = encode(PrepareMessage{"mimir", {three, ballot(4, 1), {bytes("k")}}});
  bytes_of[0] = 2;  // a version it does not speak
  EXPECT_THROW(decode_prepare(bytes_of), net::DecodeError);
  auto truncated = encode(PrepareMessage{"mimir", {three, ballot(4, 1), {bytes("k")}}});
  truncated.pop_back();
  EXPECT_THROW(decode_prepare(truncated), net::DecodeError);
  // A set whose nodes are out of order is not a set Dannan makes.
  ReplicaSet unsorted{{node(3), node(1)}, 1};
  EXPECT_THROW(decode_prepare(encode(PrepareMessage{"mimir", {unsorted, ballot(4, 1), {bytes("k")}}})), net::DecodeError);
  // No names, or trailing bytes.
  EXPECT_THROW(decode_prepare(encode(PrepareMessage{"mimir", {three, ballot(4, 1), {}}})), net::DecodeError);
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
