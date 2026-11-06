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
#include <string>

#include "dannan/status/log.h"

namespace dannan::status {

// CPU this thread has used, in nanoseconds; zero where the platform cannot say.
uint64_t thread_cpu_ns() noexcept;

// Reports a thread's CPU use, under its pool's name: Macha's DIAG lines, a pool
// here where Macha has a thread per connection. At ALL every period; at DEBUG
// only a sustained hot thread (10%), again each minute, and once when it has
// recovered (5%), so the diagnostic stays quiet. One per thread: not thread-safe.
class ThreadCpuReporter {
 public:
  using Clock = std::chrono::steady_clock;
  ThreadCpuReporter(
      std::string name, std::chrono::milliseconds interval, ScopedLogger log, bool report_idle = false, std::function<uint64_t()> cpu_ns = thread_cpu_ns,
      std::function<Clock::time_point()> now = [] { return Clock::now(); });

  // A unit of work done, or a wake with none: reports if a period has passed.
  void tick(uint64_t iterations = 1);

 private:
  const std::string _name;
  const std::chrono::milliseconds _interval;
  const ScopedLogger _log;
  const bool _report_idle;
  const std::function<uint64_t()> _cpu_ns;
  const std::function<Clock::time_point()> _now;
  Clock::time_point _wall_started;
  uint64_t _cpu_started;
  uint64_t _iterations = 0;
  bool _hot = false;
  Clock::time_point _last_report{};
};

}  // namespace dannan::status
