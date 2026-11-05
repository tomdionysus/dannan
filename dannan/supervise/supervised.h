//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"

namespace dannan::supervise {

struct SupervisedThreadStatus {
  std::string name;
  // Threads of this name inside their body, and waiting out a backoff.
  size_t running = 0;
  size_t restarting = 0;
  uint64_t faults = 0;
  // Empty until the first fault: exception or unknown_exception.
  std::string last_fault_code;
  std::string last_fault;
  uint64_t last_fault_unix_ms = 0;
};

// The entry points for every thread body a node or a hosted system runs: an
// exception escaping a thread would end the process; these catch it at the
// boundary, record it by name, and do what the thread's kind says. One per
// node, injected; names stay listed once seen. Thread-safe.
class SupervisedThreads {
 public:
  explicit SupervisedThreads(std::chrono::milliseconds initial = std::chrono::seconds(1), std::chrono::milliseconds ceiling = std::chrono::seconds(60),
                             const contract::WallClock* wall = &contract::system_wall_clock(),
                             const contract::TimeSource* time = &contract::steady_time_source(), std::shared_ptr<status::LogSink> log = nullptr)
      : _initial(initial), _ceiling(ceiling), _wall(wall), _time(time), _log(log ? std::move(log) : status::null_sink(), "supervise") {}

  // A service loop: `body` runs until `stop`; if it throws it runs again
  // after a backoff (doubling from initial to the ceiling, back to initial
  // after a run longer than the ceiling). A body that returns has finished.
  void loop(std::string_view name, std::stop_token stop, const std::function<void()>& body) noexcept;
  // A task that runs once: a fault is recorded and the thread ends; its
  // owner decides what the failure means.
  void once(std::string_view name, const std::function<void()>& body) noexcept;
  // A supervised subsystem's thread: a fault is recorded and handed to
  // `escalate`, which rebuilds the whole instance.
  void escalating(std::string_view name, const std::function<void()>& body, const std::function<void(std::string)>& escalate) noexcept;

  std::vector<SupervisedThreadStatus> statuses() const;

  // The delay after `consecutive` faults (1-based).
  static std::chrono::milliseconds restart_delay(size_t consecutive, std::chrono::milliseconds initial, std::chrono::milliseconds ceiling) noexcept;

 private:
  // Runs body; the fault's description, empty if it returned.
  std::string run(std::string_view name, const std::function<void()>& body) noexcept;
  void adjust(std::string_view name, int running, int restarting);
  void say(std::string_view name, std::string_view outcome) noexcept;

  const std::chrono::milliseconds _initial;
  const std::chrono::milliseconds _ceiling;
  const contract::WallClock* const _wall;
  const contract::TimeSource* const _time;
  const status::ScopedLogger _log;
  mutable contract::Mutex _mutex;
  std::map<std::string, SupervisedThreadStatus, std::less<>> _statuses DANNAN_GUARDED_BY(_mutex);
};

// A service loop under `threads`, or, for a component given none (a test's,
// usually), under a record nobody reads: either way nothing escapes the thread.
void supervised_loop(SupervisedThreads* threads, std::string_view name, std::stop_token stop, const std::function<void()>& body) noexcept;

}  // namespace dannan::supervise
