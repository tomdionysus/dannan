//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/process.h"

#include <pthread.h>
#include <unistd.h>

#include <fstream>
#include <string>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace dannan::status {

uint64_t resident_bytes() noexcept {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0;
  return info.resident_size;
#elif defined(__linux__)
  try {
    std::ifstream statm("/proc/self/statm");
    uint64_t pages = 0, resident = 0;
    if (!(statm >> pages >> resident)) return 0;
    return resident * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
  } catch (...) {
    return 0;
  }
#else
  return 0;
#endif
}

uint64_t physical_memory_bytes() noexcept {
#if defined(__APPLE__)
  uint64_t bytes = 0;
  size_t size = sizeof(bytes);
  return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
#elif defined(__linux__)
  const auto pages = sysconf(_SC_PHYS_PAGES);
  const auto page = sysconf(_SC_PAGESIZE);
  return pages > 0 && page > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page) : 0;
#else
  return 0;
#endif
}

void set_thread_name(std::string_view name) noexcept {
  try {
#if defined(__APPLE__)
    (void)pthread_setname_np(std::string(name.substr(0, 63)).c_str());
#elif defined(__linux__)
    (void)pthread_setname_np(pthread_self(), std::string(name.substr(0, 15)).c_str());
#else
    (void)name;
#endif
  } catch (...) {
  }
}

}  // namespace dannan::status
