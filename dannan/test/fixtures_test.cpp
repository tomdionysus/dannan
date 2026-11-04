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

// What process tests of a command need: its environment replaced, bytes on
// its standard input, its raw output read to the end, a terminal for its
// output, and any signal.
TEST(ChildProcess, TakesAnEnvironmentInputAndATerminal) {
  TempDir dir;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  ChildOptions only;
  only.environment = std::vector<std::string>{"ONLY=this"};
  ChildProcess env({"/usr/bin/env"}, dir.path() / "env.log", only);
  EXPECT_EQ(env.read_all(deadline), "ONLY=this\n");
  EXPECT_EQ(env.wait(deadline), 0);

  ChildOptions typed;
  typed.input = std::string("first\nsecond\n");
  ChildProcess cat({"/bin/cat"}, dir.path() / "cat.log", typed);
  EXPECT_EQ(cat.read_all(deadline), "first\nsecond\n");
  EXPECT_EQ(cat.wait(deadline), 0);

  // Raw bytes, line ends among them, are not split.
  ChildProcess raw({"/usr/bin/printf", "a\\nb\\000c"}, dir.path() / "raw.log");
  EXPECT_EQ(raw.read_all(deadline), std::string("a\nb\0c", 5));

  ChildOptions on_terminal;
  on_terminal.terminal = true;
  ChildProcess tty({"/bin/sh", "-c", "if [ -t 1 ]; then echo terminal; else echo pipe; fi"}, dir.path() / "tty.log", on_terminal);
  EXPECT_EQ(tty.line(deadline), "terminal");
  ChildProcess piped({"/bin/sh", "-c", "if [ -t 1 ]; then echo terminal; else echo pipe; fi"}, dir.path() / "piped.log");
  EXPECT_EQ(piped.line(deadline), "pipe");

  ChildProcess interrupted({"/bin/sleep", "30"}, dir.path() / "sleep.log");
  interrupted.signal(SIGINT);
  EXPECT_EQ(interrupted.wait(deadline), 128 + SIGINT);

  // A child that never reads its input does not hold the test.
  ChildOptions ignored;
  ignored.input = std::string(1 << 20, 'x');
  ChildProcess deaf({"/bin/sleep", "30"}, dir.path() / "deaf.log", ignored);
}

}  // namespace
}  // namespace dannan::test
