//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A wait on injected time (0.12.0, the notifying TimeSource; Dagda's stalls under TSan): on a source
// that says when it moves no timer is armed, so a frozen clock with thousands of waits costs
// nothing; on any other source one timer polls by wake_within. Posted, never inline; cancelled,
// never run.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <vector>

#include "dannan/contract/time_wait.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::contract {
namespace {

using namespace std::chrono_literals;

// io.run() returns at once when nothing is armed: no handler, no timer.
TEST(TimeWait, OnAWatchingSourceNothingIsArmedUntilTimeReachesIt) {
  boost::asio::io_context io;
  test::SteppedTime time;
  int fired = 0;
  const auto wait = TimeWait::start(io.get_executor(), time, time.now() + 1s, [&] { ++fired; });
  EXPECT_EQ(io.run(), 0u);
  EXPECT_EQ(time.watchers(), 1u);
  time.advance(999ms);
  io.restart();
  EXPECT_EQ(io.run(), 0u);
  EXPECT_EQ(fired, 0);
  time.advance(1ms);
  // Told on this thread, run only on the executor.
  EXPECT_EQ(fired, 0);
  EXPECT_TRUE(wait->over());
  io.restart();
  io.run();
  EXPECT_EQ(fired, 1);
  EXPECT_EQ(time.watchers(), 0u);
  time.advance(1h);
  io.restart();
  io.run();
  EXPECT_EQ(fired, 1);
}

TEST(TimeWait, AThousandWaitsOnAFrozenClockArmNothing) {
  boost::asio::io_context io;
  test::SteppedTime time;
  int fired = 0;
  for (int i = 0; i < 1000; ++i) (void)TimeWait::start(io.get_executor(), time, time.now() + std::chrono::seconds(1 + i % 10), [&] { ++fired; });
  // Held by their watches, not by the caller.
  EXPECT_EQ(io.run(), 0u);
  EXPECT_EQ(time.watchers(), 1000u);
  time.advance(5s);
  io.restart();
  io.run();
  EXPECT_EQ(fired, 500);
  time.advance(5s);
  io.restart();
  io.run();
  EXPECT_EQ(fired, 1000);
  EXPECT_EQ(time.watchers(), 0u);
}

TEST(TimeWait, ATimeAlreadyReachedFiresPostedNotInline) {
  boost::asio::io_context io;
  test::SteppedTime time;
  int fired = 0;
  (void)TimeWait::start(io.get_executor(), time, time.now(), [&] { ++fired; });
  EXPECT_EQ(fired, 0);
  io.run();
  EXPECT_EQ(fired, 1);
  EXPECT_EQ(time.watchers(), 0u);
}

TEST(TimeWait, ACancelledWaitNeverRuns) {
  boost::asio::io_context io;
  test::SteppedTime time;
  int fired = 0;
  const auto wait = TimeWait::start(io.get_executor(), time, time.now() + 1s, [&] { ++fired; });
  wait->cancel();
  EXPECT_TRUE(wait->over());
  EXPECT_EQ(time.watchers(), 0u);
  time.advance(2s);
  io.run();
  EXPECT_EQ(fired, 0);
  wait->cancel();
}

// The real clock says nothing: one timer, exact.
TEST(TimeWait, OnTheRealClockOneTimerWaits) {
  boost::asio::io_context io;
  const auto& time = steady_time_source();
  int fired = 0;
  const auto due = time.now() + 30ms;
  (void)TimeWait::start(io.get_executor(), time, due, [&] { ++fired; });
  io.run();
  EXPECT_EQ(fired, 1);
  EXPECT_GE(time.now(), due);
}

// A source that neither watches nor follows the real clock: polled by wake_within, cancelled while
// its timer is armed.
TEST(TimeWait, ASourceThatDoesNotWatchIsPolledAndMayBeCancelled) {
  struct Polled final : TimeSource {
    std::atomic<Clock::rep> at{Clock::now().time_since_epoch().count()};
    TimePoint now() const noexcept override { return TimePoint(Clock::duration(at.load())); }
  } time;
  boost::asio::io_context io;
  int fired = 0;
  const auto due = time.now() + 1h;
  (void)TimeWait::start(io.get_executor(), time, due, [&] { ++fired; });
  const auto cancelled = TimeWait::start(io.get_executor(), time, due, [&] { fired += 100; });
  // A few polls with the time standing, then it moves past.
  for (int i = 0; i < 3; ++i) io.run_one();
  EXPECT_EQ(fired, 0);
  cancelled->cancel();
  time.at.fetch_add(std::chrono::duration_cast<Clock::duration>(2h).count());
  io.run();
  EXPECT_EQ(fired, 1);
}

}  // namespace
}  // namespace dannan::contract
