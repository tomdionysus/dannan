//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The never-parking retry policy (TODO/2026-11-20-stack-plan.md, item 15; Louhi, Lugh, Dagda):
// for work that must go on trying (a token source, a key source, an agent's reconnect), with the
// same backoff to a ceiling and no failure budget.

#include <gtest/gtest.h>

#include <chrono>

#include "dannan/sched/retry.h"

namespace dannan::sched {
namespace {

using namespace std::chrono_literals;

TEST(RetryPolicyUnbounded, NeverParksHoweverLongItFails) {
  const auto policy = RetryPolicy::unbounded(100ms, 5s);
  RetryState state;
  contract::TimePoint now{std::chrono::hours(1)};
  for (int i = 0; i < 10'000; ++i) {
    const auto delay = state.failed(policy, now);
    ASSERT_TRUE(delay) << "parked at failure " << i;
    now += *delay;
  }
  EXPECT_EQ(state.consecutive_failures(), 10'000u);
}

TEST(RetryPolicyUnbounded, BacksOffFromTheInitialToTheCeiling) {
  const auto policy = RetryPolicy::unbounded(100ms, 1s);
  RetryState state;
  contract::TimePoint now{std::chrono::hours(1)};
  EXPECT_EQ(state.failed(policy, now), 100ms);
  EXPECT_EQ(state.failed(policy, now), 200ms);
  EXPECT_EQ(state.failed(policy, now), 400ms);
  EXPECT_EQ(state.failed(policy, now), 800ms);
  EXPECT_EQ(state.failed(policy, now), 1s);
  EXPECT_EQ(state.failed(policy, now), 1s);
}

TEST(RetryPolicyUnbounded, ManyFailuresAtOneInstantStillNeverPark) {
  const auto policy = RetryPolicy::unbounded(10ms, 10ms);
  RetryState state;
  const contract::TimePoint now{std::chrono::hours(1)};
  for (int i = 0; i < 1000; ++i) ASSERT_TRUE(state.failed(policy, now));
}

TEST(RetryPolicyUnbounded, KeepsTheFieldsInTheirOrder) {
  // Louhi brace-initialises RetryPolicy positionally (agent.h:90): the order is API.
  const RetryPolicy positional{7, 2min, 3s, 30s, 4h};
  EXPECT_EQ(positional.max_failures_in_window, 7u);
  EXPECT_EQ(positional.failure_window, 2min);
  EXPECT_EQ(positional.initial_backoff, 3s);
  EXPECT_EQ(positional.max_backoff, 30s);
  EXPECT_EQ(positional.max_failing_duration, 4h);
}

}  // namespace
}  // namespace dannan::sched
