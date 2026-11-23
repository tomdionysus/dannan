//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// The two clocks every component is given: steady time to decide by, and wall time to record.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>

namespace dannan::contract {

/// The steady clock every injected TimeSource's points are on.
using Clock = std::chrono::steady_clock;
/// A point in a TimeSource's time.
using TimePoint = Clock::time_point;

/// The steady time a component decides by: when a cached answer expires, when a deadline has
/// passed, when a lease is due.
///
/// Why injected: a test hands in a stepped source (test::SteppedTime) and moves it, rather than
/// sleeping, so a test of a 30-second deadline takes no time and gives the same answer every run.
/// Time that is only measured for a log line stays on the real clock.
///
/// Ownership: handed to a component at construction and owned by whoever constructed it, which
/// keeps it alive for the component's lifetime. Every member is thread-safe.
class TimeSource {
 public:
  virtual ~TimeSource() = default;

  /// The time now: monotonic, never decreasing. An atomic read; waits on nothing.
  virtual TimePoint now() const noexcept = 0;

  /// How long a timer should sleep, with `remaining` left in this source's time, before it looks
  /// again. A source that does not follow the real clock cannot say when its time will pass, so a
  /// waiter on it looks often (5 ms by default); the real clock's answer is exact, so production
  /// keeps one timer per wait. A source that watches (watch()) spares its waiters this.
  /// @param remaining how much of this source's time is left until the wait's point.
  virtual std::chrono::milliseconds wake_within(std::chrono::milliseconds remaining) const noexcept {
    return std::min(remaining, std::chrono::milliseconds(5));
  }

  /// Says when this source's time moves, for one whose time moves only when told (a stepped or
  /// frozen test clock), so a waiter arms no timer while it stands (0.12.0). TimeWait uses it.
  ///
  /// Why: thousands of waiters polling a frozen clock every 5 ms starved a shared io_context
  /// under TSan (Dagda, 2026-11-20).
  /// @param moved called after each move, on the thread that moved the time. It must be quick,
  ///   never throw, and take no lock the mover may hold.
  /// @return an id for unwatch(), or 0: this source does not say (the real clock does not), and
  ///   its waiters poll by wake_within().
  virtual uint64_t watch(std::function<void()> moved) const {
    (void)moved;
    return 0;
  }
  /// Stops a watch. Once it returns, `moved` is not begun again; a call already under way on
  /// another thread may finish. May be called from inside a `moved`.
  /// @param id what watch() returned; 0, or an id already unwatched, does nothing.
  virtual void unwatch(uint64_t id) const noexcept { (void)id; }
};

/// The process's steady clock: exact, so each wait on it is one timer. Stateless.
class SteadyTimeSource final : public TimeSource {
 public:
  /// The steady clock's reading.
  TimePoint now() const noexcept override { return Clock::now(); }
  /// Exactly `remaining`: the real clock's time passes on its own.
  std::chrono::milliseconds wake_within(std::chrono::milliseconds remaining) const noexcept override { return remaining; }
};

/// The one SteadyTimeSource every component may share for the life of the process. Never
/// destroyed, so a thread still running at exit (one owned by a static) never reads a clock
/// already gone.
inline const TimeSource& steady_time_source() noexcept {
  static const SteadyTimeSource* const source = new SteadyTimeSource();
  return *source;
}

/// Wall-clock time since the Unix epoch, for what is recorded or gossiped as a date: when a node
/// was last heard of, when a reset was made, a stored row's timestamp. Never used to decide a
/// deadline: the wall can be set back, and the steady TimeSource cannot. Tests hand in a stepped
/// one (test::SteppedWall). Every member waits on nothing and is thread-safe.
class WallClock {
 public:
  virtual ~WallClock() = default;
  /// Milliseconds since the Unix epoch.
  virtual uint64_t unix_ms() const noexcept = 0;
  /// Microseconds since the Unix epoch (0.12.0; for MySQL's NOW(6) and DATETIME(6), Kvasir). A
  /// clock that keeps only milliseconds answers unix_ms() * 1000, so every WallClock written
  /// before it still builds.
  virtual uint64_t unix_us() const noexcept { return unix_ms() * 1000; }
};

/// The system's wall clock.
class SystemWallClock final : public WallClock {
 public:
  /// The system clock in milliseconds.
  uint64_t unix_ms() const noexcept override {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  }
  /// The system clock in microseconds.
  uint64_t unix_us() const noexcept override {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  }
};

/// The one SystemWallClock every component may share. Never destroyed, as steady_time_source().
inline const WallClock& system_wall_clock() noexcept {
  static const SystemWallClock* const clock = new SystemWallClock();
  return *clock;
}

}  // namespace dannan::contract
