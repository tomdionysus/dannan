//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>

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
};

// The process's steady clock. Stateless.
class SteadyTimeSource final : public TimeSource {
 public:
  TimePoint now() const noexcept override { return Clock::now(); }
};

// One stateless instance serves every component for the life of the process.
inline const TimeSource& steady_time_source() noexcept {
  static const SteadyTimeSource source;
  return source;
}

}  // namespace dannan::contract
