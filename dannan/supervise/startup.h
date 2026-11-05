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
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"

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

// Watches a startup from its own thread and ends the process when it stalls:
// the stuck thread may never be joinable, so unwinding is unsafe, and the
// process supervisor starts a fresh instance whose recovery resolves the state.
// The hosted system starts it, and calls finish() when the startup is done; a
// node never ends its process unless something asked for this. `stalled` is
// for tests that observe the stall and live; by default, std::_Exit(1).
class StartupWatch {
 public:
  struct Options {
    std::chrono::milliseconds no_progress{120000};
    std::chrono::milliseconds ceiling{0};
    // What the startup is waiting for, for the line.
    std::function<std::string()> describe;
    std::function<void(const std::string& diagnostic)> stalled;
    const contract::TimeSource* time = &contract::steady_time_source();
  };

  StartupWatch(const StartupProgress& progress, status::ScopedLogger log, Options options);
  // Finishes first. Waits for the thread.
  ~StartupWatch();
  StartupWatch(const StartupWatch&) = delete;
  StartupWatch& operator=(const StartupWatch&) = delete;

  // The startup is done; the watch ends. Waits for the thread; thread-safe.
  void finish();

 private:
  void watch(std::stop_token stop);

  const StartupProgress& _progress;
  const status::ScopedLogger _log;
  const Options _options;
  // Guards nothing: the thread's wait lock.
  std::mutex _wait_mutex;
  std::condition_variable_any _wake;
  std::jthread _thread;
};

}  // namespace dannan::supervise
