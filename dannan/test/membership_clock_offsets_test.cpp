//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Each peer's clock offset from clock probes, and the rule that takes a node out of bounds
// (TODO/2026-11-20-stack-plan.md, item 6): its offset past the bound against more than half of the
// agreed roster, itself counted at zero. No special case by size: one or two nodes can never reach
// that majority against themselves.

#include <gtest/gtest.h>

#include <chrono>
#include <set>

#include "dannan/membership/clock_offsets.h"

namespace dannan::membership {
namespace {

using namespace std::chrono_literals;

identity::NodeId id(uint8_t n) {
  identity::NodeId out;
  out.bytes[0] = n;
  return out;
}

// A probe sent at `sent` (this node's wall) that the peer answered at `answered` (its wall) and
// that came back `rtt` later.
void probe(ClockOffsets& offsets, uint8_t peer, uint64_t sent, int64_t peer_offset_ms, uint64_t rtt) {
  const auto answered = static_cast<uint64_t>(static_cast<int64_t>(sent + rtt / 2) + peer_offset_ms);
  offsets.record(id(peer), sent, answered, sent + rtt);
}

TEST(ClockOffsets, OneProbeGivesTheOffsetHalfwayThroughTheRoundTrip) {
  ClockOffsets offsets;
  probe(offsets, 2, 1'000'000, 3'000, 20);
  const auto offset = offsets.offset(id(2));
  ASSERT_TRUE(offset);
  EXPECT_EQ(*offset, 3000ms);
  EXPECT_FALSE(offsets.offset(id(3)));
}

TEST(ClockOffsets, TheShortestRoundTripOfTheRecentProbesIsBelieved) {
  ClockOffsets offsets;
  // A slow, asymmetric round trip reads the offset wrongly; the quick one reads it rightly.
  offsets.record(id(2), 1'000'000, 1'000'000 + 900, 1'000'000 + 1000);
  probe(offsets, 2, 1'010'000, 50, 4);
  EXPECT_EQ(*offsets.offset(id(2)), 50ms);
}

TEST(ClockOffsets, OnlyTheRecentProbesAreKept) {
  ClockOffsets offsets;
  probe(offsets, 2, 1'000'000, 9'000, 1);  // quick, but old
  for (size_t i = 0; i < ClockOffsets::window; ++i) probe(offsets, 2, 1'100'000 + i * 1000, 0, 10);
  EXPECT_EQ(*offsets.offset(id(2)), 0ms);
}

TEST(ClockOffsets, AForgottenPeerHasNoOffset) {
  ClockOffsets offsets;
  probe(offsets, 2, 1'000'000, 10, 2);
  offsets.forget(id(2));
  EXPECT_FALSE(offsets.offset(id(2)));
}

TEST(ClockOffsets, OutOfBoundsWhenPastTheBoundAgainstMoreThanHalfTheRoster) {
  ClockOffsets offsets;
  const std::set<identity::NodeId> roster{id(1), id(2), id(3), id(4), id(5)};
  // This node (1) is three seconds behind 2, 3 and 4.
  for (uint8_t peer : {2, 3, 4}) probe(offsets, peer, 1'000'000, 3'000, 10);
  probe(offsets, 5, 1'000'000, 0, 10);
  EXPECT_TRUE(offsets.out_of_bounds(id(1), roster, 2s));
}

TEST(ClockOffsets, NotOutOfBoundsWhenHalfOrFewerDisagree) {
  ClockOffsets offsets;
  const std::set<identity::NodeId> roster{id(1), id(2), id(3), id(4)};
  for (uint8_t peer : {2, 3}) probe(offsets, peer, 1'000'000, -3'000, 10);
  probe(offsets, 4, 1'000'000, 0, 10);
  // Two of four is not more than half.
  EXPECT_FALSE(offsets.out_of_bounds(id(1), roster, 2s));
}

TEST(ClockOffsets, OneAndTwoNodesAreNeverOutOfBoundsByTheSameRule) {
  ClockOffsets alone;
  EXPECT_FALSE(alone.out_of_bounds(id(1), {id(1)}, 2s));
  ClockOffsets pair;
  probe(pair, 2, 1'000'000, 60'000, 10);
  EXPECT_FALSE(pair.out_of_bounds(id(1), {id(1), id(2)}, 2s));
}

TEST(ClockOffsets, APeerNotYetProbedDoesNotCountAgainst) {
  ClockOffsets offsets;
  const std::set<identity::NodeId> roster{id(1), id(2), id(3)};
  probe(offsets, 2, 1'000'000, 5'000, 10);
  EXPECT_FALSE(offsets.out_of_bounds(id(1), roster, 2s));
  probe(offsets, 3, 1'000'000, 5'000, 10);
  EXPECT_TRUE(offsets.out_of_bounds(id(1), roster, 2s));
}

TEST(ClockOffsets, AnOffsetAtTheBoundIsWithinIt) {
  ClockOffsets offsets;
  const std::set<identity::NodeId> roster{id(1), id(2), id(3)};
  for (uint8_t peer : {2, 3}) probe(offsets, peer, 1'000'000, 2'000, 10);
  EXPECT_FALSE(offsets.out_of_bounds(id(1), roster, 2s));
}

TEST(ClockProbe, RoundTripsOnTheWire) {
  EXPECT_EQ(decode_probe(encode_probe(1'793'700'000'123ull)), 1'793'700'000'123ull);
  const auto answer = decode_answer(encode_answer({5, 9}));
  EXPECT_EQ(answer.sent, 5u);
  EXPECT_EQ(answer.answered, 9u);
  EXPECT_EQ(encode_probe(1).size(), 8u);
  EXPECT_EQ(encode_answer({1, 2}).size(), 16u);
}

TEST(ClockProbe, AnythingElseIsADecodeError) {
  EXPECT_THROW(decode_probe(net::Bytes(7)), net::DecodeError);
  EXPECT_THROW(decode_probe(net::Bytes(9)), net::DecodeError);
  EXPECT_THROW(decode_answer(net::Bytes(15)), net::DecodeError);
  EXPECT_THROW(decode_answer(net::Bytes(17)), net::DecodeError);
}

TEST(ClockOffsets, ARoundTripThatEndsBeforeItBeganIsRefused) {
  ClockOffsets offsets;
  offsets.record(id(2), 1'000'000, 1'000'005, 999'990);
  EXPECT_FALSE(offsets.offset(id(2)));
}

}  // namespace
}  // namespace dannan::membership
