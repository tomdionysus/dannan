//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/cpu_pacing.h"

#include <time.h>

#include <ctime>

namespace dannan::sched {

CpuSample system_cpu_sample() noexcept {
  CpuSample out;
  out.process_seconds = static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
  timespec own{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &own) == 0) out.own_seconds = static_cast<double>(own.tv_sec) + static_cast<double>(own.tv_nsec) / 1e9;
  return out;
}

}  // namespace dannan::sched
