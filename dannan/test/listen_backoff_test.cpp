//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A sign-in's back-off (Lugh's, moved here so Lugh and Kvasir link one copy: Kvasir's ask,
// 2026-11-16, stack rule 3). Lugh's five cases, on Dannan's injected time point.

#include <gtest/gtest.h>

#include <string>

#include "dannan/listen/backoff.h"

namespace dannan::listen {
namespace {

using namespace std::chrono_literals;

const contract::TimePoint start{std::chrono::hours(1)};

TEST(Backoff, FreeFailuresThenDoublingWaits) {
  Backoff backoff(Backoff::Rules{});
  auto now = start;
  for (int i = 0; i < 4; ++i) {
    backoff.failed("a", now);
    EXPECT_FALSE(backoff.wait("a", now)) << i;
  }
  backoff.failed("a", now);
  EXPECT_EQ(backoff.wait("a", now), 1s);
  backoff.failed("a", now);
  EXPECT_EQ(backoff.wait("a", now), 2s);
  backoff.failed("a", now);
  EXPECT_EQ(backoff.wait("a", now), 4s);

  now += 3s;
  EXPECT_EQ(backoff.wait("a", now), 1s);
  now += 1s;
  EXPECT_FALSE(backoff.wait("a", now));
}

TEST(Backoff, TheWaitHasACeiling) {
  Backoff backoff(Backoff::Rules{});
  for (int i = 0; i < 100; ++i) backoff.failed("a", start);
  EXPECT_EQ(backoff.wait("a", start), std::chrono::seconds(15min));
}

TEST(Backoff, KeysAreSeparateAndSuccessClearsOne) {
  Backoff backoff(Backoff::Rules{});
  for (int i = 0; i < 6; ++i) {
    backoff.failed("a", start);
    backoff.failed("b", start);
  }
  backoff.succeeded("a");
  EXPECT_FALSE(backoff.wait("a", start));
  EXPECT_TRUE(backoff.wait("b", start));
}

TEST(Backoff, QuietKeysAreForgotten) {
  Backoff backoff(Backoff::Rules{});
  for (int i = 0; i < 20; ++i) backoff.failed("a", start);
  EXPECT_FALSE(backoff.wait("a", start + 1h));
  // And start again from nothing.
  backoff.failed("a", start + 1h);
  EXPECT_FALSE(backoff.wait("a", start + 1h));
}

TEST(Backoff, MadeUpKeysDoNotGrowWithoutBound) {
  Backoff backoff(Backoff::Rules{}, 100);
  for (int i = 0; i < 150; ++i) backoff.failed("old" + std::to_string(i), start);
  for (int i = 0; i < 150; ++i) backoff.failed("new" + std::to_string(i), start + 2h);
  EXPECT_LE(backoff.size(), 152u);
}

// The bound is hard (item 10; Kvasir's proof): a flood of made-up names that are all fresh, so
// none is forgotten, still never holds more than the bound, and a real name under attack keeps
// its penalty: the keys with fewest failures go first.
TEST(Backoff, AFloodOfFreshNamesStaysBoundedAndARealPenaltyStays) {
  Backoff backoff(Backoff::Rules{}, 100);
  for (int i = 0; i < 12; ++i) backoff.failed("real", start);
  const auto penalty = backoff.wait("real", start);
  ASSERT_TRUE(penalty);
  for (int i = 0; i < 10000; ++i) {
    backoff.failed("made-up-" + std::to_string(i), start + std::chrono::milliseconds(i));
    ASSERT_LE(backoff.size(), 100u) << i;
  }
  EXPECT_EQ(backoff.wait("real", start), penalty);
}

// Among keys with as many failures, the one failed longest ago goes first.
TEST(Backoff, AtTheBoundTheOldestOfTheFewestFailuresGoesFirst) {
  Backoff::Rules rules;
  rules.free_failures = 0;
  Backoff backoff(rules, 8);
  backoff.failed("oldest", start);
  for (int i = 0; i < 7; ++i) backoff.failed("k" + std::to_string(i), start + std::chrono::seconds(1 + i));
  backoff.failed("newest", start + 10s);
  EXPECT_LE(backoff.size(), 8u);
  EXPECT_FALSE(backoff.wait("oldest", start + 10s));
  EXPECT_TRUE(backoff.wait("newest", start + 10s));
}

}  // namespace
}  // namespace dannan::listen
