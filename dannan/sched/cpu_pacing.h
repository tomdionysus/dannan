//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <functional>

// Background work's budget against what the rest of the process is using
// (Macha's background_cpu_scale, maintenance.cpp): above the target it loses
// budget continuously rather than stopping in a fixed block-per-tick pattern.
// Dagda's store/cpu_pacing.h, moved here as it is (P4), so Dagda's copy is a
// delete and an include.
namespace dannan::sched {

// CPU seconds since some fixed point: the whole process's, and the calling
// thread's own, which is taken out so background work is not paced by itself.
struct CpuSample {
  double process_seconds = 0.0;
  double own_seconds = 0.0;
};
using CpuSource = std::function<CpuSample()>;
// The real clocks. Waits on nothing.
CpuSample system_cpu_sample() noexcept;

// The fraction of its budget background work keeps over `wall_seconds`.
inline double background_cpu_scale(double process_seconds, double own_seconds, double wall_seconds, double cpu_target) noexcept {
  if (wall_seconds <= 0.0) return 1.0;
  const double load = std::max(0.0, process_seconds - own_seconds) / wall_seconds;
  if (load <= cpu_target) return 1.0;
  return std::clamp(cpu_target / load, 0.0, 1.0);
}

// Differences successive samples into a scale. Single owner.
class CpuPacer {
 public:
  explicit CpuPacer(CpuSource source) : _source(std::move(source)) {
    if (_source) _last = _source();
  }
  // Nothing to read: always 1.
  double scale(double wall_seconds, double cpu_target) {
    if (!_source) return 1.0;
    const auto now = _source();
    const double process = now.process_seconds - _last.process_seconds;
    const double own = now.own_seconds - _last.own_seconds;
    _last = now;
    return background_cpu_scale(process, own, wall_seconds, cpu_target);
  }

 private:
  CpuSource _source;
  CpuSample _last{};
};

}  // namespace dannan::sched
