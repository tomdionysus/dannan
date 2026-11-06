//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/thread_cpu.h"

#include <time.h>

#include <cmath>
#include <cstdlib>
#include <utility>

namespace dannan::status {
namespace {

constexpr double high_percent = 10.0;
constexpr double recovered_percent = 5.0;
constexpr auto high_repeat = std::chrono::seconds(60);

std::string message(std::string_view prefix, const std::string& name, int64_t wall_ms, uint64_t cpu_ms, double percent, uint64_t iterations) {
  const auto tenths = static_cast<long long>(std::llround(percent * 10.0));
  return std::string(prefix) + " name=" + name + " wall_ms=" + std::to_string(wall_ms) + " cpu_ms=" + std::to_string(cpu_ms) +
         " cpu_pct=" + std::to_string(tenths / 10) + "." + std::to_string(std::llabs(tenths % 10)) + " iterations=" + std::to_string(iterations);
}

}  // namespace

uint64_t thread_cpu_ns() noexcept {
  timespec now{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) return 0;
  return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(now.tv_nsec);
}

ThreadCpuReporter::ThreadCpuReporter(std::string name, std::chrono::milliseconds interval, ScopedLogger log, bool report_idle, std::function<uint64_t()> cpu_ns,
                                     std::function<Clock::time_point()> now)
    : _name(std::move(name)),
      _interval(interval),
      _log(std::move(log)),
      _report_idle(report_idle),
      _cpu_ns(std::move(cpu_ns)),
      _now(std::move(now)),
      _wall_started(_now()),
      _cpu_started(_cpu_ns()) {}

void ThreadCpuReporter::tick(uint64_t iterations) {
  _iterations += iterations;
  const auto now = _now();
  if (now - _wall_started < _interval) return;
  const auto cpu = _cpu_ns();
  const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - _wall_started).count();
  const auto cpu_ns = cpu >= _cpu_started ? cpu - _cpu_started : 0;
  const double percent = wall_ns > 0 ? 100.0 * static_cast<double>(cpu_ns) / static_cast<double>(wall_ns) : 0.0;
  const int64_t wall_ms = wall_ns / 1'000'000;
  const uint64_t cpu_ms = cpu_ns / 1'000'000;
  if (_log.enabled(LogLevel::all) && (_report_idle || percent >= 1.0 || _iterations > 0))
    _log.trace(message("DIAG thread", _name, wall_ms, cpu_ms, percent, _iterations));
  if (!_log.enabled(LogLevel::all) && _log.enabled(LogLevel::debug)) {
    if (percent >= high_percent) {
      if (!_hot || now - _last_report >= high_repeat) {
        _log.debug(message("DIAG high thread CPU", _name, wall_ms, cpu_ms, percent, _iterations));
        _last_report = now;
      }
      _hot = true;
    } else if (_hot && percent <= recovered_percent) {
      _log.debug(message("DIAG thread CPU recovered", _name, wall_ms, cpu_ms, percent, _iterations));
      _hot = false;
    }
  }
  _wall_started = now;
  _cpu_started = cpu;
  _iterations = 0;
}

}  // namespace dannan::status
