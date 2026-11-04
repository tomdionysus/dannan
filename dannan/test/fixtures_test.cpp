//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/ports.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::test {
namespace {

TEST(Gate, HoldsEveryEnteringThreadUntilOpened) {
  Gate gate;
  GateOpener opener(gate);
  std::atomic<int> passed{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i)
    threads.emplace_back([&] {
      gate.enter_and_wait();
      ++passed;
    });
  ASSERT_TRUE(gate.wait_for_entries(3));
  EXPECT_EQ(passed.load(), 0);
  gate.open();
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(passed.load(), 3);
}

TEST(Gate, ReportsEntriesThatNeverArrive) {
  Gate gate;
  EXPECT_FALSE(gate.wait_for_entries(1, std::chrono::milliseconds(0)));
  EXPECT_EQ(gate.entered(), 0u);
}

TEST(Gate, AnOpenGateLetsLaterEntriesThrough) {
  Gate gate;
  gate.open();
  gate.enter_and_wait();
  EXPECT_EQ(gate.entered(), 1u);
}

TEST(FreePort, NeverHandsOutTheSamePortTwice) {
  std::set<uint16_t> seen;
  for (int i = 0; i < 32; ++i) EXPECT_TRUE(seen.insert(free_port()).second);
}

TEST(FreePort, TheHandedOutPortCanBeBound) {
  const uint16_t port = free_port();
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  EXPECT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  ::close(fd);
}

// A child process: its lines read by deadline, its exit status, and how a
// signal ended it.
TEST(ChildProcess, ReadsLinesAndReportsHowItEnded) {
  TempDir dir;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  ChildProcess speaks({"/bin/sh", "-c", "echo one; echo two; exit 3"}, dir.path() / "speaks.log");
  EXPECT_EQ(speaks.line(deadline), "one");
  EXPECT_EQ(speaks.line(deadline), "two");
  EXPECT_FALSE(speaks.line(deadline));  // end of output
  EXPECT_EQ(speaks.wait(deadline), 3);
  EXPECT_EQ(speaks.wait(deadline), 3);
  ChildProcess sleeper({"/bin/sleep", "30"}, dir.path() / "sleeper.log");
  EXPECT_FALSE(sleeper.wait(std::chrono::steady_clock::now() + std::chrono::milliseconds(100)));
  sleeper.terminate();
  EXPECT_EQ(sleeper.wait(deadline), 128 + SIGTERM);
  EXPECT_THROW(ChildProcess({"/no/such/binary"}, dir.path() / "none.log"), std::runtime_error);
}

}  // namespace
}  // namespace dannan::test
