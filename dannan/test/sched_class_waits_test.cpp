//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// ClassWaits. The retained memory ledger's cases prove it under the ledger; these
// prove the rules the store's DATA credit needs from it can be written on it, from
// Macha's DataResourceArbiter (src/cluster/data_work.hpp): three asymmetric classes,
// an outside predicate for pressure, a count beside the bytes, and no-progress
// windows any release re-arms. The arbiter's own cases, in the store, are its gate.

#include <gtest/gtest.h>

#include <condition_variable>
#include <future>
#include <thread>

#include "dannan/sched/class_waits.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::sched {
namespace {

using namespace std::chrono_literals;

// The arbiter's admission rules, as a test's policy over ClassWaits.
struct Arbiter {
  explicit Arbiter(uint64_t capacity, uint64_t reserve, uint64_t ceiling = 0, const contract::TimeSource* time = &contract::steady_time_source())
      : capacity(capacity), reserve(reserve), ceiling(ceiling), classes(time) {}
  const uint64_t capacity;
  const uint64_t reserve;
  const uint64_t ceiling;
  bool pressured = false;
  bool viewer_recent = false;
  uint64_t min_background = 1;
  contract::Mutex mutex;
  std::condition_variable_any changed;
  ClassWaits classes;
  uint64_t used = 0;
  uint64_t lower_used = 0;
  uint64_t lower_active = 0;
  uint64_t refusals = 0;
  bool stopping = false;

  bool available(WorkClass work, uint64_t bytes, bool* refused) {
    *refused = false;
    if (used > capacity - bytes) return false;
    if (work == WorkClass::foreground) return true;
    if (classes.waiting(WorkClass::foreground)) return false;
    if (pressured && lower_active >= min_background) {
      const bool present = used > lower_used || viewer_recent;
      if (work == WorkClass::speculative || present) {
        *refused = true;
        return false;
      }
    }
    if (ceiling && lower_active >= ceiling) return false;
    const auto lower = capacity - reserve;
    if (bytes > lower || lower_used > lower - bytes) return false;
    return !(work == WorkClass::speculative && classes.waiting(WorkClass::loader));
  }

  ClassWaits::Outcome acquire(WorkClass work, uint64_t bytes, ClassWaits::Bounds bounds = {}) {
    contract::Lock lock(mutex);
    bool refused = false;
    const auto outcome = classes.wait(
        lock, changed, work, bounds, [&] { return stopping; }, [&] { return available(work, bytes, &refused); },
        [&] {
          if (refused) ++refusals;
        },
        [](contract::Lock&) { return false; });
    if (outcome == ClassWaits::Outcome::admitted) {
      used += bytes;
      if (work != WorkClass::foreground) {
        lower_used += bytes;
        ++lower_active;
      }
    }
    return outcome;
  }

  void release(WorkClass work, uint64_t bytes) {
    {
      contract::Lock lock(mutex);
      used -= bytes;
      if (work != WorkClass::foreground) {
        lower_used -= bytes;
        --lower_active;
      }
      classes.released();
    }
    changed.notify_all();
  }

  uint64_t waiting(WorkClass work) {
    contract::Lock lock(mutex);
    return classes.waiting(work);
  }

  void set(const std::function<void()>& change) {
    {
      contract::Lock lock(mutex);
      change();
    }
    changed.notify_all();
  }
};

bool eventually(const std::function<bool()>& condition) {
  for (int spin = 0; spin < 2000; ++spin) {
    if (condition()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return condition();
}

ClassWaits::Bounds at_most(std::chrono::milliseconds window) {
  ClassWaits::Bounds bounds;
  bounds.deadline = contract::Clock::now() + window;
  return bounds;
}

TEST(ClassWaits, ForegroundMayUseTheReserveAndLowerClassesMayNot) {
  Arbiter arbiter(100, 20);
  EXPECT_EQ(arbiter.acquire(WorkClass::loader, 80), ClassWaits::Outcome::admitted);
  EXPECT_EQ(arbiter.acquire(WorkClass::speculative, 1, at_most(20ms)), ClassWaits::Outcome::gave_up);
  EXPECT_EQ(arbiter.acquire(WorkClass::foreground, 20), ClassWaits::Outcome::admitted);
}

TEST(ClassWaits, AWaitingForegroundClosesBothLowerClassesAndAWaitingLoaderOnlySpeculative) {
  Arbiter arbiter(100, 20);
  ASSERT_EQ(arbiter.acquire(WorkClass::foreground, 100), ClassWaits::Outcome::admitted);
  auto viewer = std::async(std::launch::async, [&] { return arbiter.acquire(WorkClass::foreground, 90); });
  ASSERT_TRUE(eventually([&] { return arbiter.waiting(WorkClass::foreground) == 1; }));
  arbiter.release(WorkClass::foreground, 80);
  // Room for a loader, but a foreground waits.
  EXPECT_EQ(arbiter.acquire(WorkClass::loader, 10, at_most(20ms)), ClassWaits::Outcome::gave_up);
  arbiter.release(WorkClass::foreground, 20);
  EXPECT_EQ(viewer.get(), ClassWaits::Outcome::admitted);
  arbiter.release(WorkClass::foreground, 90);
  ASSERT_EQ(arbiter.acquire(WorkClass::loader, 80), ClassWaits::Outcome::admitted);
  auto loader = std::async(std::launch::async, [&] { return arbiter.acquire(WorkClass::loader, 5); });
  ASSERT_TRUE(eventually([&] { return arbiter.waiting(WorkClass::loader) == 1; }));
  // A waiting loader closes speculative, not foreground.
  EXPECT_EQ(arbiter.acquire(WorkClass::speculative, 1, at_most(20ms)), ClassWaits::Outcome::gave_up);
  EXPECT_EQ(arbiter.acquire(WorkClass::foreground, 1), ClassWaits::Outcome::admitted);
  arbiter.release(WorkClass::loader, 80);
  EXPECT_EQ(loader.get(), ClassWaits::Outcome::admitted);
}

// The pressure gate asks something outside the mechanism: the loader yields only
// when a foreground user is present, speculative to pressure alone, and neither
// below the minimum, so background always drains.
TEST(ClassWaits, AnOutsidePredicateDecidesPressure) {
  Arbiter arbiter(1000, 100);
  arbiter.pressured = true;
  EXPECT_EQ(arbiter.acquire(WorkClass::speculative, 1), ClassWaits::Outcome::admitted);  // under the minimum
  EXPECT_EQ(arbiter.acquire(WorkClass::speculative, 1, at_most(20ms)), ClassWaits::Outcome::gave_up);
  EXPECT_EQ(arbiter.refusals, 1u);
  EXPECT_EQ(arbiter.acquire(WorkClass::loader, 1), ClassWaits::Outcome::admitted);  // nobody is watching
  arbiter.viewer_recent = true;
  EXPECT_EQ(arbiter.acquire(WorkClass::loader, 1, at_most(20ms)), ClassWaits::Outcome::gave_up);
  EXPECT_EQ(arbiter.refusals, 2u);
  EXPECT_EQ(arbiter.acquire(WorkClass::foreground, 1), ClassWaits::Outcome::admitted);  // never refused for it
}

TEST(ClassWaits, ACountCeilingStandsBesideTheBytes) {
  Arbiter arbiter(1000, 100, 2);
  EXPECT_EQ(arbiter.acquire(WorkClass::loader, 1), ClassWaits::Outcome::admitted);
  EXPECT_EQ(arbiter.acquire(WorkClass::loader, 1), ClassWaits::Outcome::admitted);
  EXPECT_EQ(arbiter.acquire(WorkClass::loader, 1, at_most(20ms)), ClassWaits::Outcome::gave_up);
  EXPECT_EQ(arbiter.acquire(WorkClass::foreground, 1), ClassWaits::Outcome::admitted);
}

// With no deadline a wait gives up only if nothing anywhere has been released for a
// whole window; any release re-arms it.
TEST(ClassWaits, AnyReleaseReArmsTheNoProgressWindow) {
  test::SteppedTime time;
  Arbiter arbiter(100, 20, 0, &time);
  ASSERT_EQ(arbiter.acquire(WorkClass::foreground, 100), ClassWaits::Outcome::admitted);
  ClassWaits::Bounds bounds;
  bounds.no_progress = 1h;
  auto waiter = std::async(std::launch::async, [&] { return arbiter.acquire(WorkClass::foreground, 100, bounds); });
  ASSERT_TRUE(eventually([&] { return arbiter.waiting(WorkClass::foreground) == 1; }));
  for (int i = 0; i < 3; ++i) {
    time.advance(50min);
    // A release elsewhere: not enough for this waiter, but progress.
    arbiter.release(WorkClass::foreground, 1);
    arbiter.set([&] { arbiter.used += 1; });
    const auto reads = time.reads();
    ASSERT_TRUE(eventually([&] { return time.reads() > reads + 5; }));
    EXPECT_EQ(waiter.wait_for(0ms), std::future_status::timeout);
  }
  time.advance(61min);
  EXPECT_EQ(waiter.get(), ClassWaits::Outcome::no_progress);
  contract::Lock lock(arbiter.mutex);
  EXPECT_EQ(arbiter.classes.no_progress(), 1u);
  EXPECT_EQ(arbiter.classes.waiting(WorkClass::foreground), 0u);
}

TEST(ClassWaits, ACancelIsSeenAtThePollAndAStopAtOnce) {
  Arbiter arbiter(100, 20);
  ASSERT_EQ(arbiter.acquire(WorkClass::foreground, 100), ClassWaits::Outcome::admitted);
  std::atomic_bool cancelled{false};
  ClassWaits::Bounds bounds;
  bounds.cancelled = &cancelled;
  bounds.cancellation_poll = 5ms;
  auto waiter = std::async(std::launch::async, [&] { return arbiter.acquire(WorkClass::loader, 1, bounds); });
  ASSERT_TRUE(eventually([&] { return arbiter.waiting(WorkClass::loader) == 1; }));
  cancelled = true;
  EXPECT_EQ(waiter.get(), ClassWaits::Outcome::gave_up);
  auto stopped = std::async(std::launch::async, [&] { return arbiter.acquire(WorkClass::loader, 1); });
  ASSERT_TRUE(eventually([&] { return arbiter.waiting(WorkClass::loader) == 1; }));
  arbiter.set([&] { arbiter.stopping = true; });
  EXPECT_EQ(stopped.get(), ClassWaits::Outcome::gave_up);
}

}  // namespace
}  // namespace dannan::sched
