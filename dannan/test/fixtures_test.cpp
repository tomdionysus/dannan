//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <string>
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

// Below every kernel's ephemeral range (Linux from 32768, macOS from 49152),
// so an outgoing connection elsewhere on the host is never given a port a
// test chose and has yet to bind.
TEST(FreePort, ComesFromBelowTheEphemeralRange) {
  for (int i = 0; i < 32; ++i) {
    const auto port = free_port();
    EXPECT_GE(port, 10000);
    EXPECT_LT(port, 32768);
  }
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

// Whether `pid` is a process still running: not gone, and on Linux not a
// zombie nobody has reaped (a container's first process may never reap one).
bool running(pid_t pid) {
  if (::kill(pid, 0) != 0) return false;
#if defined(__linux__)
  std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
  std::string line;
  if (!std::getline(stat, line)) return false;
  const auto close = line.rfind(')');
  return close == std::string::npos || close + 2 >= line.size() || line[close + 2] != 'Z';
#else
  return true;
#endif
}

// A test process that dies without running its destructors (a crash, an
// abort, SIGKILL) leaves no child running: each child's reaper ends it.
TEST(ChildProcess, AChildOutlivesNoTestProcessThatDies) {
  TempDir dir;
  int report[2];
  ASSERT_EQ(::pipe(report), 0);
  const pid_t test = ::fork();
  ASSERT_GE(test, 0);
  if (test == 0) {
    ::close(report[0]);
    ChildProcess child({"/bin/sleep", "30"}, dir.path() / "orphan.log");
    const pid_t pid = child.pid();
    if (::write(report[1], &pid, sizeof(pid)) != sizeof(pid)) ::_exit(2);
    ::raise(SIGKILL);
    ::_exit(3);
  }
  ::close(report[1]);
  pid_t child = 0;
  pollfd ready{report[0], POLLIN, 0};
  ASSERT_EQ(::poll(&ready, 1, 10000), 1);
  ASSERT_EQ(::read(report[0], &child, sizeof(child)), static_cast<ssize_t>(sizeof(child)));
  ::close(report[0]);
  int status = 0;
  ASSERT_EQ(::waitpid(test, &status, 0), test);
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (running(child) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(running(child));
  if (running(child)) ::kill(child, SIGKILL);
}

// Once the test has waited for a child, the reaper stands down without
// signalling anything, so a pid the system has handed on is never hit; the
// child's status is what it was.
TEST(ChildProcess, AReaperStandsDownOnceTheChildIsWaitedFor) {
  TempDir dir;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  ChildProcess quick({"/bin/sh", "-c", "exit 7"}, dir.path() / "quick.log");
  EXPECT_EQ(quick.wait(deadline), 7);
  EXPECT_EQ(quick.reaper_status(), 0);
  ChildProcess ended({"/bin/sleep", "30"}, dir.path() / "ended.log");
  ended.terminate();
  EXPECT_EQ(ended.wait(deadline), 128 + SIGTERM);
  EXPECT_EQ(ended.reaper_status(), 0);
}

// Children started on several threads at once, each with its reaper forked
// from a threaded process: every one starts, says its line, and ends as it
// should, with its reaper standing down.
TEST(ChildProcess, ChildrenStartOnManyThreadsAtOnce) {
  TempDir dir;
  constexpr int count = 8;
  std::atomic<int> good{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < count; ++i)
    threads.emplace_back([&, i] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      ChildProcess child({"/bin/sh", "-c", "echo " + std::to_string(i)}, dir.path() / ("many-" + std::to_string(i) + ".log"));
      if (child.line(deadline) == std::to_string(i) && child.wait(deadline) == 0 && child.reaper_status() == 0) ++good;
    });
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(good.load(), count);
}

}  // namespace
}  // namespace dannan::test
