//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The duty share by weight (Dagda asked, 2026-11-04; Macha's weighted loader
// service, inventory: background pacing): work-conserving while nothing
// contends, a slice then a rest in proportion while something does, and
// never starving the background. Time is passed in, so nothing waits.

#include <gtest/gtest.h>

#include <stdexcept>

#include "dannan/sched/duty_share.h"

namespace dannan::sched {
namespace {

using namespace std::chrono_literals;

const contract::TimePoint t0{};

TEST(DutyShare, WeightsAndSliceArePositive) {
  EXPECT_THROW(DutyShare(0, 5), std::invalid_argument);
  EXPECT_THROW(DutyShare(95, 0), std::invalid_argument);
  EXPECT_THROW(DutyShare(95, 5, 0ms), std::invalid_argument);
}

// Nothing contends: background work runs without pause, and an earlier
// rest is forgotten.
TEST(DutyShare, RunsFreelyWhileNothingContends) {
  DutyShare share(95, 5, 25ms);
  share.started(t0, true);
  EXPECT_EQ(share.finished(t0 + 25ms, true), 475ms);
  EXPECT_FALSE(share.may_start(t0 + 30ms, true));
  EXPECT_TRUE(share.may_start(t0 + 30ms, false));
  share.started(t0 + 30ms, false);
  EXPECT_FALSE(share.should_yield(t0 + 10s, false));
  EXPECT_EQ(share.finished(t0 + 10s, false), 0ms);
  EXPECT_EQ(share.wait(t0 + 10s, true), 0ms);
}

// Contended: one slice, then a rest of slice x 95/5; the background gets
// its turn again after it, so it is paced, never stopped.
TEST(DutyShare, ASliceThenARestInProportion) {
  DutyShare share(95, 5, 25ms);
  ASSERT_TRUE(share.may_start(t0, true));
  share.started(t0, true);
  EXPECT_FALSE(share.should_yield(t0 + 24ms, true));
  EXPECT_TRUE(share.should_yield(t0 + 25ms, true));
  EXPECT_EQ(share.finished(t0 + 25ms, true), 475ms);
  EXPECT_FALSE(share.may_start(t0 + 499ms, true));
  EXPECT_EQ(share.wait(t0 + 400ms, true), 100ms);
  EXPECT_TRUE(share.may_start(t0 + 500ms, true));
  // A burst that ends early rests in proportion to what it used.
  share.started(t0 + 500ms, true);
  EXPECT_EQ(share.finished(t0 + 510ms, true), 190ms);
}

// Contention arriving mid-burst ends it at the next check, and the rest is
// charged from there.
TEST(DutyShare, ContentionMidBurstYieldsAtOnce) {
  DutyShare share(95, 5, 25ms);
  share.started(t0, false);
  EXPECT_TRUE(share.should_yield(t0 + 10ms, true));
  EXPECT_EQ(share.finished(t0 + 20ms, true), 190ms);
  EXPECT_FALSE(share.may_start(t0 + 209ms, true));
  EXPECT_TRUE(share.may_start(t0 + 210ms, true));
}

// Setup is counted as active but not charged: a slow setup would otherwise
// spend the slice and be multiplied into the rest.
TEST(DutyShare, SetupIsNotCharged) {
  DutyShare share(95, 5, 25ms);
  share.started(t0, true, false);
  share.serving(t0 + 10s, true);
  EXPECT_FALSE(share.should_yield(t0 + 10s + 24ms, true));
  EXPECT_TRUE(share.should_yield(t0 + 10s + 25ms, true));
  EXPECT_EQ(share.finished(t0 + 10s + 25ms, true), 475ms);
}

// Workers sharing one burst: the rest is charged once, when the last ends.
TEST(DutyShare, TheLastWorkerChargesTheRest) {
  DutyShare share(3, 1, 25ms);
  share.started(t0, true);
  share.started(t0 + 5ms, true);
  EXPECT_EQ(share.finished(t0 + 20ms, true), 0ms);
  EXPECT_EQ(share.finished(t0 + 25ms, true), 75ms);
}

// However long a burst ran, its rest is bounded: never starved for good.
TEST(DutyShare, ARestIsBounded) {
  DutyShare share(1000000, 1, 25ms);
  share.started(t0, true);
  EXPECT_EQ(share.finished(t0 + 1h, true), std::chrono::milliseconds(24h));
}

}  // namespace
}  // namespace dannan::sched
