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

namespace dannan::supervise {

// A node's startup progress, ticked by each unit of long recovery work. One
// per node, injected. Lock-free.
class StartupProgress {
 public:
  void note(uint64_t units = 1) noexcept { _count.fetch_add(units, std::memory_order_relaxed); }
  uint64_t value() const noexcept { return _count.load(std::memory_order_relaxed); }

 private:
  std::atomic<uint64_t> _count{0};
};

// Decides when a startup has stalled: no progress for `no_progress`, or
// `ceiling` since it began; zero turns either off. A startup is stopped for
// making no progress, never merely for being slow. Fed each reading with the
// time it was taken; single owner.
class StartupStallGate {
 public:
  StartupStallGate(std::chrono::milliseconds no_progress, std::chrono::milliseconds ceiling, contract::TimePoint started, uint64_t progress)
      : _no_progress(no_progress), _ceiling(ceiling), _started(started), _last_progress_at(started), _last_progress(progress) {}

  bool stalled(contract::TimePoint now, uint64_t progress) {
    if (progress != _last_progress) {
      _last_progress = progress;
      _last_progress_at = now;
    }
    return (_no_progress.count() > 0 && now - _last_progress_at >= _no_progress) || (_ceiling.count() > 0 && now - _started >= _ceiling);
  }
  // A second, or a quarter of a shorter no-progress window, so a stall is
  // seen within a quarter window of it.
  std::chrono::milliseconds poll_interval() const {
    constexpr std::chrono::milliseconds most{1000};
    if (_no_progress.count() <= 0) return most;
    return std::clamp(_no_progress / 4, std::chrono::milliseconds{1}, most);
  }
  contract::TimePoint started() const noexcept { return _started; }
  contract::TimePoint last_progress_at() const noexcept { return _last_progress_at; }

 private:
  const std::chrono::milliseconds _no_progress;
  const std::chrono::milliseconds _ceiling;
  const contract::TimePoint _started;
  contract::TimePoint _last_progress_at;
  uint64_t _last_progress;
};

}  // namespace dannan::supervise
