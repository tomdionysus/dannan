//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The wall clock in microseconds (0.12.0; Kvasir, for MySQL's NOW(6) and DATETIME(6)): the system
// clock gives real microseconds, a clock that keeps only milliseconds answers them times a
// thousand, and the stepped test clock steps in microseconds.

#include <gtest/gtest.h>

#include "dannan/contract/time_source.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::contract {
namespace {

using namespace std::chrono_literals;

TEST(WallClock, TheSystemClockGivesMicrosecondsAgreeingWithItsMilliseconds) {
  const auto& wall = system_wall_clock();
  const auto before_ms = wall.unix_ms();
  const auto us = wall.unix_us();
  const auto after_ms = wall.unix_ms();
  EXPECT_GE(us / 1000, before_ms);
  EXPECT_LE(us / 1000, after_ms);
  // Not merely milliseconds times a thousand: across a thousand reads some fall between.
  bool between = false;
  for (int i = 0; i < 1000 && !between; ++i) between = wall.unix_us() % 1000 != 0;
  EXPECT_TRUE(between);
}

TEST(WallClock, AMillisecondClockAnswersMicrosecondsTimesAThousand) {
  struct Fixed final : WallClock {
    uint64_t unix_ms() const noexcept override { return 1793700000123ull; }
  } fixed;
  EXPECT_EQ(fixed.unix_us(), 1793700000123000ull);
}

TEST(WallClock, TheSteppedClockStepsInMicroseconds) {
  test::SteppedWall wall(1000);
  EXPECT_EQ(wall.unix_us(), 1000000u);
  wall.advance(250us);
  EXPECT_EQ(wall.unix_us(), 1000250u);
  EXPECT_EQ(wall.unix_ms(), 1000u);
  wall.advance(750us);
  EXPECT_EQ(wall.unix_ms(), 1001u);
  wall.advance(2ms);
  EXPECT_EQ(wall.unix_us(), 1003000u);
}

}  // namespace
}  // namespace dannan::contract
