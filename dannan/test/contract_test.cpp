//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Injected time and published snapshots: the inventory's rows "injected time
// for every interval decision" and "published immutable snapshots behind a
// swap".

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "dannan/contract/published.h"
#include "dannan/contract/scope.h"
#include "dannan/contract/time_source.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::contract {
namespace {

TEST(SteadyTimeSource, NeverGoesBackwards) {
  const auto& source = steady_time_source();
  auto last = source.now();
  for (int i = 0; i < 1000; ++i) {
    const auto now = source.now();
    EXPECT_GE(now, last);
    last = now;
  }
}

TEST(SteppedTime, StandsStillUntilAdvanced) {
  test::SteppedTime time;
  const auto start = time.now();
  EXPECT_EQ(time.now(), start);
  time.advance(std::chrono::seconds(30));
  EXPECT_EQ(time.now(), start + std::chrono::seconds(30));
}

// A test waits for another thread to have read the clock before moving it.
TEST(SteppedTime, CountsReads) {
  test::SteppedTime time;
  const auto before = time.reads();
  std::thread reader([&] { (void)time.now(); });
  reader.join();
  EXPECT_EQ(time.reads(), before + 1);
}

TEST(Published, IsEmptyBeforeTheFirstPublish) {
  Published<std::string> published;
  EXPECT_EQ(published.handle(), nullptr);
}

// A publish never changes what a held handle reads.
TEST(Published, AHeldHandleKeepsItsSnapshot) {
  Published<std::string> published(std::make_shared<const std::string>("first"));
  const auto held = published.handle();
  published.publish(std::string("second"));
  EXPECT_EQ(*held, "first");
  EXPECT_EQ(*published.handle(), "second");
}

TEST(Published, RefusesToWithdrawTheSnapshot) {
  Published<int> published(std::make_shared<const int>(1));
  EXPECT_THROW(published.publish(std::shared_ptr<const int>{}), std::invalid_argument);
  EXPECT_EQ(*published.handle(), 1);
}

// The previous snapshot dies with its last holder, not under the lock.
TEST(Published, TheOldSnapshotLivesUntilItsLastHolderLetsGo) {
  Published<int> published(std::make_shared<const int>(1));
  std::weak_ptr<const int> old = published.handle();
  {
    const auto held = published.handle();
    published.publish(2);
    EXPECT_FALSE(old.expired());
  }
  EXPECT_TRUE(old.expired());
}

// A held lock released for a scope (device I/O in the middle of a section)
// and retaken after: another thread can take the mutex meanwhile.
TEST(Unlocked, ReleasesForItsScopeAndRetakes) {
  struct Guarded {
    Mutex mutex;
    int value DANNAN_GUARDED_BY(mutex) = 0;
  } state;
  Lock lock(state.mutex);
  state.value = 1;
  {
    Unlocked unlocked(lock, state.mutex);
    std::thread other([&] {
      Lock theirs(state.mutex);
      state.value = 2;
    });
    other.join();
  }
  EXPECT_TRUE(lock.owns_lock());
  EXPECT_EQ(state.value, 2);
}

TEST(Principal, ComparesByKindAndId) {
  const Principal node{Principal::Kind::node, "0123"};
  EXPECT_EQ(node, (Principal{Principal::Kind::node, "0123"}));
  EXPECT_NE(node, (Principal{Principal::Kind::directory, "0123"}));
  EXPECT_NE(node, (Principal{Principal::Kind::node, "4567"}));
}

}  // namespace
}  // namespace dannan::contract
