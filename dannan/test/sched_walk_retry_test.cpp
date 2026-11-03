//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Budgets, cursors and pages for paged long work, and the retry policy:
// the inventory's "Work classes, waits and budgets" table; disciplines 2
// and 4 (no wait without a deadline; long work is paged).

#include <gtest/gtest.h>

#include <atomic>

#include "dannan/sched/retry.h"
#include "dannan/sched/walk.h"

namespace dannan::sched {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

const TimePoint t0 = TimePoint{} + seconds(1000);

TEST(Budget, AnAbsentBoundNeverStopsAWalk) {
  Budget budget;
  for (int i = 0; i < 10000; ++i) EXPECT_TRUE(budget.take_operation());
  EXPECT_TRUE(budget.take_bytes(UINT64_MAX));
  EXPECT_FALSE(budget.must_stop(t0).has_value());
}

TEST(Budget, OperationsAreSpentOneAtATimeAndThenRefused) {
  Budget budget;
  budget.operations(2);
  EXPECT_TRUE(budget.take_operation());
  EXPECT_TRUE(budget.take_operation());
  EXPECT_FALSE(budget.take_operation());
  EXPECT_EQ(budget.operations_left(), 0u);
}

TEST(Budget, ARefusedByteTakeSpendsNothing) {
  Budget budget;
  budget.bytes(100);
  EXPECT_TRUE(budget.take_bytes(60));
  EXPECT_FALSE(budget.take_bytes(41));
  EXPECT_EQ(budget.bytes_left(), 40u);
  EXPECT_TRUE(budget.take_bytes(40));
  EXPECT_EQ(budget.bytes_left(), 0u);
}

class Yield final : public YieldSource {
 public:
  bool should_yield() const override { return yield; }
  bool yield = false;
};

// Cancellation first, then either deadline, then the yield source.
TEST(Budget, MustStopNamesTheFirstReasonInOrder) {
  std::atomic_bool cancelled{false};
  Yield yield;
  Budget budget(WorkContext(FrameType::speculative, t0 + seconds(10), &cancelled, "walk"));
  budget.deadline(t0 + seconds(5)).yield_to(&yield);
  EXPECT_FALSE(budget.must_stop(t0).has_value());
  yield.yield = true;
  EXPECT_EQ(budget.must_stop(t0), Stop::yield);
  EXPECT_EQ(budget.must_stop(t0 + seconds(5)), Stop::deadline);
  cancelled = true;
  EXPECT_EQ(budget.must_stop(t0 + seconds(5)), Stop::cancelled);
}

TEST(Budget, TheContextsDeadlineStopsAWalkWithoutItsOwn) {
  Budget budget(WorkContext(FrameType::loader, t0 + seconds(1), nullptr, "walk"));
  EXPECT_FALSE(budget.must_stop(t0).has_value());
  EXPECT_EQ(budget.must_stop(t0 + seconds(1)), Stop::deadline);
}

TEST(Page, IsCompleteOnlyWhenTheWalkReachedTheEnd) {
  Page<int, int> page;
  EXPECT_TRUE(page.complete());
  page.stopped = Stop::budget;
  EXPECT_FALSE(page.complete());
  Cursor<int> start;
  Cursor<int> after_seven{7};
  EXPECT_FALSE(start.after.has_value());
  EXPECT_NE(start, after_seven);
}

const RetryPolicy policy{3, seconds(60), milliseconds(100), milliseconds(1000), seconds(600)};

TEST(Retry, BacksOffExponentiallyToTheCeiling) {
  RetryState state;
  EXPECT_EQ(state.failed(policy, t0), milliseconds(100));
  EXPECT_EQ(state.failed(policy, t0 + seconds(30)), milliseconds(200));
  RetryState wide;
  const RetryPolicy many{100, seconds(1), milliseconds(100), milliseconds(1000), seconds(0)};
  milliseconds last{};
  for (int i = 0; i < 10; ++i) last = *wide.failed(many, t0 + seconds(2 * i));
  EXPECT_EQ(last, milliseconds(1000));
}

TEST(Retry, IsDueOnlyAfterItsBackoff) {
  RetryState state;
  EXPECT_TRUE(state.due(t0));
  const auto backoff = *state.failed(policy, t0);
  EXPECT_FALSE(state.due(t0 + backoff - milliseconds(1)));
  EXPECT_TRUE(state.due(t0 + backoff));
}

// More failures than the budget inside the window parks the item.
TEST(Retry, ParksPastTheFailureBudgetInTheWindow) {
  RetryState state;
  EXPECT_TRUE(state.failed(policy, t0).has_value());
  EXPECT_TRUE(state.failed(policy, t0 + seconds(1)).has_value());
  EXPECT_TRUE(state.failed(policy, t0 + seconds(2)).has_value());
  EXPECT_FALSE(state.failed(policy, t0 + seconds(3)).has_value());
}

TEST(Retry, FailuresOutsideTheWindowDoNotCount) {
  RetryState state;
  for (int i = 0; i < 10; ++i) EXPECT_TRUE(state.failed(policy, t0 + seconds(61 * i)).has_value()) << i;
}

// The backstop: unbroken failure past max_failing_duration parks however
// sparse the retries.
TEST(Retry, ParksAnUnbrokenRunPastTheBackstop) {
  RetryState state;
  EXPECT_TRUE(state.failed(policy, t0).has_value());
  EXPECT_TRUE(state.failed(policy, t0 + seconds(300)).has_value());
  EXPECT_FALSE(state.failed(policy, t0 + seconds(601)).has_value());
}

TEST(Retry, ASuccessResetsTheBackoffButKeepsTheWindow) {
  RetryState state;
  state.failed(policy, t0);
  state.failed(policy, t0 + seconds(1));
  state.succeeded();
  EXPECT_EQ(state.consecutive_failures(), 0u);
  EXPECT_TRUE(state.due(t0 + seconds(1)));
  EXPECT_EQ(state.failed(policy, t0 + seconds(2)), milliseconds(100));
  // Three in the window: a flapping item still parks.
  EXPECT_FALSE(state.failed(policy, t0 + seconds(3)).has_value());
}

TEST(Retry, ResetForgetsEverything) {
  RetryState state;
  for (int i = 0; i < 4; ++i) state.failed(policy, t0 + seconds(i));
  state.reset();
  EXPECT_EQ(state.total_failures(), 0u);
  EXPECT_EQ(state.failures_in_window(), 0u);
  EXPECT_TRUE(state.failed(policy, t0 + seconds(5)).has_value());
}

}  // namespace
}  // namespace dannan::sched
