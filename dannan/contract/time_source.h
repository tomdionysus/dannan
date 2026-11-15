//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace dannan::contract {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// The steady time a component decides by: when a cached answer expires, when
// a deadline has passed, when a lease is due. Handed to the component at
// construction and owned by whoever constructed it, which keeps it alive for
// the component's lifetime. Time only measured for logs stays on the real
// clock. Tests hand in a stepped source and advance it instead of sleeping.
class TimeSource {
 public:
  virtual ~TimeSource() = default;

  // Monotonic, non-decreasing; an atomic read. Waits on nothing; thread-safe.
  virtual TimePoint now() const noexcept = 0;

  // How long a timer should sleep with `remaining` left in this source's time
  // before looking again. A source that does not follow the real clock cannot
  // say when its time will pass, so the waiter looks often; the real clock's
  // answer is exact, so production keeps one timer per wait.
  virtual std::chrono::milliseconds wake_within(std::chrono::milliseconds remaining) const noexcept {
    return std::min(remaining, std::chrono::milliseconds(5));
  }
};

// The process's steady clock. Stateless.
class SteadyTimeSource final : public TimeSource {
 public:
  TimePoint now() const noexcept override { return Clock::now(); }
  std::chrono::milliseconds wake_within(std::chrono::milliseconds remaining) const noexcept override { return remaining; }
};

// One stateless instance serves every component for the life of the process.
// Never destroyed, so a thread still running at exit (one owned by a static)
// never reads a clock already gone.
inline const TimeSource& steady_time_source() noexcept {
  static const SteadyTimeSource* const source = new SteadyTimeSource();
  return *source;
}

// Wall-clock milliseconds since the Unix epoch, for what is recorded or
// gossiped as a date (when a node was last heard of, when a reset was made).
// Never used to decide a deadline: that is the steady TimeSource's. Tests
// hand in a stepped one.
class WallClock {
 public:
  virtual ~WallClock() = default;
  // Waits on nothing; thread-safe.
  virtual uint64_t unix_ms() const noexcept = 0;
};

class SystemWallClock final : public WallClock {
 public:
  uint64_t unix_ms() const noexcept override {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  }
};

// Never destroyed, as steady_time_source().
inline const WallClock& system_wall_clock() noexcept {
  static const SystemWallClock* const clock = new SystemWallClock();
  return *clock;
}

}  // namespace dannan::contract
