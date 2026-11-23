//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "dannan/contract/time_source.h"

namespace dannan::test {

// A TimeSource that stands still until advance() moves it. It starts at the
// steady clock's reading, so its time points compare with real ones.
//
// A waiter on it cannot know when it will move, so it looks again every `poll` of real time
// (5 ms by default). A fixture with many waiters, thousands of calls each with its watchdog, gives
// a coarser poll: at 5 ms a frozen clock's waiters can starve a shared io_context (Dagda's stalls
// under TSan, gone at 250 ms). A step is then seen within `poll`.
class SteppedTime final : public contract::TimeSource {
 public:
  explicit SteppedTime(std::chrono::milliseconds poll = std::chrono::milliseconds(5)) noexcept : _poll(poll) {}

  // Precondition: by >= 0.
  void advance(contract::Clock::duration by) noexcept { _now.fetch_add(by.count()); }
  contract::TimePoint now() const noexcept override {
    _reads.fetch_add(1);
    return contract::TimePoint(contract::Clock::duration(_now.load()));
  }
  // How many times now() has been read: a test waits for another thread to
  // have taken its reading before moving time on.
  uint64_t reads() const noexcept { return _reads.load(); }
  std::chrono::milliseconds wake_within(std::chrono::milliseconds remaining) const noexcept override { return std::min(remaining, _poll); }

 private:
  const std::chrono::milliseconds _poll;
  std::atomic<contract::Clock::rep> _now{contract::Clock::now().time_since_epoch().count()};
  mutable std::atomic<uint64_t> _reads{0};
};

// A WallClock that stands still until advance() moves it, in microseconds (0.12.0, for Kvasir's
// NOW(6)): a step in milliseconds converts as it is.
class SteppedWall final : public contract::WallClock {
 public:
  explicit SteppedWall(uint64_t start_ms = 1793700000000ull) : _now_us(start_ms * 1000) {}
  void advance(std::chrono::microseconds by) noexcept { _now_us.fetch_add(static_cast<uint64_t>(by.count())); }
  uint64_t unix_ms() const noexcept override { return _now_us.load() / 1000; }
  uint64_t unix_us() const noexcept override { return _now_us.load(); }

 private:
  std::atomic<uint64_t> _now_us;
};

}  // namespace dannan::test
