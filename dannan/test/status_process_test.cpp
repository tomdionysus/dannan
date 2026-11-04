//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// What a process knows of itself: its resident memory, the machine's, and
// its threads' names.

#include <gtest/gtest.h>
#include <pthread.h>

#include <string>
#include <thread>

#include "dannan/status/process.h"

namespace dannan::status {
namespace {

TEST(Process, KnowsItsMemory) {
  const auto resident = resident_bytes();
  const auto physical = physical_memory_bytes();
  EXPECT_GT(resident, 1u << 20);
  EXPECT_GT(physical, resident);
}

// A thread names itself; a name past the platform's limit is cut, not
// refused.
TEST(Process, NamesTheCallingThread) {
  std::string seen;
  std::thread([&] {
    set_thread_name("dannan-durability-worker");
    char name[64] = {};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    seen = name;
  }).join();
  EXPECT_EQ(std::string("dannan-durability-worker").rfind(seen, 0), 0u) << seen;
  EXPECT_GE(seen.size(), 15u);
}

}  // namespace
}  // namespace dannan::status
