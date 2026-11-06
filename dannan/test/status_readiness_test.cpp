//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Readiness, against Macha's RecoveryProgress (src/cluster/local_state.hpp):
// planes marked one by one, the first failure kept, completion that may wait for
// an explicit mark, and waiters woken by either.

#include <gtest/gtest.h>

#include <thread>

#include "dannan/status/readiness.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::status {
namespace {

using namespace std::chrono_literals;

TEST(Readiness, IsCompleteWhenEveryPlaneIsReady) {
  test::SteppedWall wall;
  Readiness readiness({"data", "metadata"}, &wall);
  EXPECT_FALSE(readiness.complete());
  readiness.ready("data");
  EXPECT_TRUE(readiness.has("data"));
  EXPECT_FALSE(readiness.has("metadata"));
  EXPECT_FALSE(readiness.complete());
  EXPECT_EQ(readiness.ready_unix_ms(), 0u);
  readiness.ready("metadata");
  EXPECT_TRUE(readiness.complete());
  EXPECT_NE(readiness.ready_unix_ms(), 0u);
  EXPECT_THROW(readiness.ready("nothing"), std::invalid_argument);
}

// Macha marks complete only after recovery returns, later than its planes, so a
// node is not ready, and its zero capacity not trusted, while recovery runs.
TEST(Readiness, HeldItWaitsForTheMarkAfterItsPlanes) {
  test::SteppedWall wall;
  Readiness readiness({"data"}, &wall);
  readiness.hold_until_marked();
  readiness.control_plane(true);
  readiness.ready("data");
  EXPECT_FALSE(readiness.complete());
  EXPECT_EQ(readiness.phase(), Phase::recovering);
  EXPECT_EQ(readiness.ready_unix_ms(), 0u);
  wall.advance(5ms);
  const auto marked_at = wall.unix_ms();
  readiness.mark_complete();
  EXPECT_TRUE(readiness.complete());
  EXPECT_EQ(readiness.phase(), Phase::ready);
  EXPECT_EQ(readiness.ready_unix_ms(), marked_at);
}

TEST(Readiness, AMarkBeforeItsPlanesDoesNotComplete) {
  test::SteppedWall wall;
  Readiness readiness({"data"}, &wall);
  readiness.hold_until_marked();
  readiness.mark_complete();
  EXPECT_FALSE(readiness.complete());
  readiness.ready("data");
  EXPECT_TRUE(readiness.complete());
}

TEST(Readiness, HoldingAfterTheNodeStartsIsRefused) {
  test::SteppedWall wall;
  Readiness readiness({}, &wall);
  readiness.close_declarations();
  EXPECT_THROW(readiness.hold_until_marked(), std::logic_error);
}

TEST(Readiness, KeepsTheFirstFailureAndWakesItsWaiters) {
  test::SteppedWall wall;
  Readiness readiness({"data"}, &wall);
  std::thread failer([&] {
    readiness.fail("the disk is gone");
    readiness.fail("and so a later thing broke");
  });
  EXPECT_FALSE(readiness.wait_complete(10s));
  failer.join();
  EXPECT_TRUE(readiness.failed());
  EXPECT_EQ(readiness.error(), "the disk is gone");
}

TEST(Readiness, AMarkWakesAWaiter) {
  test::SteppedWall wall;
  Readiness readiness({}, &wall);
  readiness.hold_until_marked();
  std::thread marker([&] { readiness.mark_complete(); });
  EXPECT_TRUE(readiness.wait_complete(10s));
  marker.join();
}

}  // namespace
}  // namespace dannan::status
