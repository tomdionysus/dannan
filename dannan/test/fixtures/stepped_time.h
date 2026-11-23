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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "dannan/contract/time_source.h"

namespace dannan::test {

// A TimeSource that stands still until advance() moves it. It starts at the
// steady clock's reading, so its time points compare with real ones.
//
// It says when it moves (watch(), 0.12.0), so a waiter that asks arms no timer while it stands:
// a frozen clock with thousands of waiters costs nothing (Dagda's stalls under TSan). A waiter
// that does not ask looks again every `poll` of real time (5 ms by default).
class SteppedTime final : public contract::TimeSource {
 public:
  explicit SteppedTime(std::chrono::milliseconds poll = std::chrono::milliseconds(5)) noexcept : _poll(poll) {}

  // Precondition: by >= 0. Every watcher is told after the move, on this thread, outside the
  // watchers' lock.
  void advance(contract::Clock::duration by) noexcept {
    _now.fetch_add(by.count());
    std::vector<std::shared_ptr<const std::function<void()>>> told;
    {
      std::lock_guard lock(_watchers_mutex);
      for (const auto& [_, watcher] : _watchers) told.push_back(watcher);
    }
    for (const auto& watcher : told) (*watcher)();
  }
  uint64_t watch(std::function<void()> moved) const override {
    std::lock_guard lock(_watchers_mutex);
    const auto id = ++_next_watcher;
    _watchers.emplace(id, std::make_shared<const std::function<void()>>(std::move(moved)));
    return id;
  }
  void unwatch(uint64_t id) const noexcept override {
    std::lock_guard lock(_watchers_mutex);
    _watchers.erase(id);
  }
  // How many watch() an advance would tell now: a test that waiters leave nothing behind.
  size_t watchers() const {
    std::lock_guard lock(_watchers_mutex);
    return _watchers.size();
  }
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
  mutable std::mutex _watchers_mutex;
  mutable std::map<uint64_t, std::shared_ptr<const std::function<void()>>> _watchers;
  mutable uint64_t _next_watcher = 0;
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
