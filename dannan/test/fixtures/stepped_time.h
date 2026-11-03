//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>

#include "dannan/contract/time_source.h"

namespace dannan::test {

// A TimeSource that stands still until advance() moves it. It starts at the
// steady clock's reading, so its time points compare with real ones.
class SteppedTime final : public contract::TimeSource {
 public:
  // Precondition: by >= 0.
  void advance(contract::Clock::duration by) noexcept { _now.fetch_add(by.count()); }
  contract::TimePoint now() const noexcept override {
    _reads.fetch_add(1);
    return contract::TimePoint(contract::Clock::duration(_now.load()));
  }
  // How many times now() has been read: a test waits for another thread to
  // have taken its reading before moving time on.
  uint64_t reads() const noexcept { return _reads.load(); }

 private:
  std::atomic<contract::Clock::rep> _now{contract::Clock::now().time_since_epoch().count()};
  mutable std::atomic<uint64_t> _reads{0};
};

// A WallClock that stands still until advance() moves it.
class SteppedWall final : public contract::WallClock {
 public:
  explicit SteppedWall(uint64_t start_ms = 1793700000000ull) : _now(start_ms) {}
  void advance(std::chrono::milliseconds by) noexcept { _now.fetch_add(static_cast<uint64_t>(by.count())); }
  uint64_t unix_ms() const noexcept override { return _now.load(); }

 private:
  std::atomic<uint64_t> _now;
};

}  // namespace dannan::test
