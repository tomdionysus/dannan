//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/work.h"

namespace dannan::sched {

// The part every limit by work class shares: who is waiting, by class, so a higher
// class's wait can close admission to the lower ones, and how a wait ends. What may
// be admitted is the caller's: its own figures, its own rules, under its own lock.
// The node's retained memory is written on this, and so are the store's DATA credit
// and device slots.
//
// Not thread-safe by itself: every call but the two readers' counts is made holding
// the one lock the caller passes in, which also guards these figures.
class ClassWaits {
 public:
  enum class Outcome {
    admitted,
    // Stopped, cancelled, or past its deadline.
    gave_up,
    // No deadline, and nothing released anywhere for the whole window.
    no_progress,
  };

  struct Bounds {
    // Default (the epoch): none.
    contract::TimePoint deadline{};
    const std::atomic_bool* cancelled = nullptr;
    // With no deadline, how long a wait may go with no release anywhere: any
    // release re-arms it, so only a wholly stalled limit gives up. Zero: no window.
    std::chrono::milliseconds no_progress{0};
    // How often to look at what nothing notifies; zero when a cancel wakes the caller's
    // condition variable.
    std::chrono::milliseconds cancellation_poll{0};
  };

  explicit ClassWaits(const contract::TimeSource* time = &contract::steady_time_source()) : _time(time) {}

  // Waits until `available()`. Each is called holding the lock:
  //   available():  may this be admitted now?
  //   first_wait(): this waiter has just begun to wait, once per waiter.
  //   before_sleep(lock): true if it let go of the lock and did something, so look
  //                 again before sleeping; a limit's way to ask holders to give back.
  // `stopping()` ends the wait as gave_up. The waiter is uncounted by the time this
  // returns. Waits on `changed`, which the caller notifies on each release. The analysis
  // cannot follow a lock through callables: the callers hold theirs by contract.
  template <typename Lock, typename Changed, typename Stopping, typename Available, typename FirstWait, typename BeforeSleep>
  Outcome wait(Lock& lock, Changed& changed, WorkClass work, const Bounds& bounds, Stopping stopping, Available available, FirstWait first_wait,
               BeforeSleep before_sleep) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
    bool counted = false;
    auto window_start = _time->now();
    auto seen = _releases;
    const auto leave = [&] {
      if (counted) --_waiting[index(work)];
    };
    for (;;) {
      const auto now = _time->now();
      if (stopping() || (bounds.cancelled && bounds.cancelled->load(std::memory_order_relaxed)) ||
          (bounds.deadline != contract::TimePoint{} && now >= bounds.deadline)) {
        leave();
        ++_gave_up;
        changed.notify_all();
        return Outcome::gave_up;
      }
      if (available()) {
        leave();
        return Outcome::admitted;
      }
      if (!counted) {
        counted = true;
        ++_waiting[index(work)];
        ++_waits[index(work)];
        first_wait();
      }
      if (before_sleep(lock)) continue;
      auto until = bounds.deadline;
      if (until == contract::TimePoint{} && bounds.no_progress.count() > 0) {
        if (_releases != seen) {
          seen = _releases;
          window_start = now;
        } else if (now - window_start >= bounds.no_progress) {
          leave();
          ++_no_progress;
          changed.notify_all();
          return Outcome::no_progress;
        }
        until = window_start + bounds.no_progress;
      }
      if (until == contract::TimePoint{} && bounds.cancellation_poll.count() == 0) {
        changed.wait(lock.native());
        continue;
      }
      std::chrono::nanoseconds slice = std::chrono::hours(24);
      if (until != contract::TimePoint{}) slice = std::max<std::chrono::nanoseconds>(until - now, std::chrono::milliseconds(1));
      if (bounds.cancellation_poll.count() > 0) slice = std::min<std::chrono::nanoseconds>(slice, bounds.cancellation_poll);
      // A source that is not the real clock moves without waking this: look often.
      slice = std::min<std::chrono::nanoseconds>(slice, _time->wake_within(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours(24))));
      changed.wait_for(lock.native(), slice);
    }
  }

  // Something was given back: restarts every no-progress window.
  void released() noexcept { ++_releases; }

  uint64_t waiting(WorkClass work) const noexcept { return _waiting[index(work)]; }
  uint64_t waits(WorkClass work) const noexcept { return _waits[index(work)]; }
  // Waits that ended without admission for a stop, a cancel or a deadline.
  uint64_t gave_up() const noexcept { return _gave_up; }
  uint64_t no_progress() const noexcept { return _no_progress; }
  const std::array<uint64_t, 4>& all_waits() const noexcept { return _waits; }

 private:
  static constexpr size_t index(WorkClass work) noexcept { return static_cast<size_t>(work); }

  const contract::TimeSource* const _time;
  std::array<uint64_t, 4> _waiting{};
  std::array<uint64_t, 4> _waits{};
  uint64_t _gave_up = 0;
  uint64_t _no_progress = 0;
  uint64_t _releases = 0;
};

}  // namespace dannan::sched
