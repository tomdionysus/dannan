//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The node-program helper (item 14; TODO/2026-11-23-node-program.md, agreed with Lugh, Mimir,
// Kvasir and Louhi), through a probe binary made of it alone: --version and --help, command words,
// the exit codes, before_start, prepare before start, the status line, SIGHUP reloading and never
// stopping, a signal draining in order (drain, on_stop, the node's stop, on_exit), a drain step
// left at its deadline, a second signal stopping at once, and node.seconds.

#include <gtest/gtest.h>
#include <signal.h>

#include <fstream>
#include <string>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "dannan/version.h"

namespace dannan::supervise {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Probe {
  test::TempDir dir;
  std::vector<std::string> argv{DANNAN_PROGRAM_PROBE};

  // A node's settings: its own state and key, an ephemeral port on loopback.
  Probe& node() {
    std::ofstream(dir.path() / "cluster.key") << "probe cluster";
    const std::vector<std::string> settings{"--cluster-key",
                                            (dir.path() / "cluster.key").string(),
                                            "--cluster-state",
                                            (dir.path() / "state").string(),
                                            "--cluster-listen-address",
                                            "127.0.0.1",
                                            "--cluster-listen-port",
                                            "0",
                                            "--cluster-listen-advertise",
                                            "127.0.0.1",
                                            "--cluster-listen-inbound",
                                            "yes"};
    argv.insert(argv.end(), settings.begin(), settings.end());
    return *this;
  }
  Probe& with(std::initializer_list<std::string> more) {
    argv.insert(argv.end(), more);
    return *this;
  }
  std::unique_ptr<test::ChildProcess> start() { return std::make_unique<test::ChildProcess>(argv, dir.path() / "probe.log"); }
};

// The next line holding `text`, by the deadline; the lines before it are kept in `seen`.
bool line_with(test::ChildProcess& child, const std::string& text, std::vector<std::string>& seen, Clock::duration within = 20s) {
  const auto deadline = Clock::now() + within;
  while (const auto line = child.line(deadline)) {
    seen.push_back(*line);
    if (line->find(text) != std::string::npos) return true;
  }
  return false;
}

std::optional<int> exit_of(test::ChildProcess& child) { return child.wait(Clock::now() + 30s); }

TEST(NodeProgram, VersionAndHelpPrintAndExit) {
  Probe probe;
  auto version = probe.with({"--version"}).start();
  EXPECT_EQ(version->line(Clock::now() + 10s), std::string("probe-node 9.9.9 (dannan ") + kVersion + ")");
  EXPECT_EQ(exit_of(*version), 0);
  Probe help_probe;
  auto help = help_probe.with({"--help"}).start();
  const auto text = help->read_all(Clock::now() + 10s);
  EXPECT_EQ(exit_of(*help), 0);
  for (const char* flag : {"--cluster-listen-port", "--node-drain-ms", "--node-status-ms", "--node-seconds", "--log-level", "--probe-drain-ms"})
    EXPECT_NE(text.find(flag), std::string::npos) << flag;
}

TEST(NodeProgram, CommandWordsRunAndAnUnknownOneIsRefused) {
  Probe hello;
  auto run = hello.with({"hello"}).start();
  EXPECT_EQ(run->line(Clock::now() + 10s), std::string("hello"));
  EXPECT_EQ(exit_of(*run), 7);
  Probe unknown;
  auto refused = unknown.with({"nope"}).start();
  EXPECT_EQ(exit_of(*refused), 2);
}

TEST(NodeProgram, ASettingThatCannotBeUsedExitsTwo) {
  Probe probe;
  auto bad = probe.with({"--node-drain-ms", "soon"}).start();
  EXPECT_EQ(exit_of(*bad), 2);
  Probe flag;
  auto unknown = flag.with({"--no-such-flag"}).start();
  EXPECT_EQ(exit_of(*unknown), 2);
}

TEST(NodeProgram, BeforeStartMayEndTheProgramWithoutANode) {
  Probe probe;
  auto child = probe.node().with({"--probe-before", "exit3"}).start();
  std::vector<std::string> seen;
  EXPECT_FALSE(line_with(*child, "probe.prepared", seen, 2s));
  EXPECT_EQ(exit_of(*child), 3);
}

// Prepared before start, then started; status lines carry the program's fields; SIGHUP reloads
// and the node runs on; SIGTERM drains in order and exits 0.
TEST(NodeProgram, ASignalDrainsInOrderAndSighupOnlyReloads) {
  Probe probe;
  auto child = probe.node().with({"--node-status-ms", "200", "--probe-drain-ms", "100"}).start();
  std::vector<std::string> seen;
  ASSERT_TRUE(line_with(*child, "probe.prepared", seen)) << "stderr in " << (probe.dir.path() / "probe.log");
  ASSERT_TRUE(line_with(*child, "probe.started", seen));
  ASSERT_TRUE(line_with(*child, "\"event\":\"node.status\"", seen));
  EXPECT_NE(seen.back().find("\"probe\":1"), std::string::npos) << seen.back();
  EXPECT_NE(seen.back().find("\"health\":"), std::string::npos) << seen.back();
  child->signal(SIGHUP);
  ASSERT_TRUE(line_with(*child, "probe.reload", seen));
  // Still running: another status line comes.
  ASSERT_TRUE(line_with(*child, "\"event\":\"node.status\"", seen));
  child->signal(SIGTERM);
  ASSERT_TRUE(line_with(*child, "probe.drain", seen));
  ASSERT_TRUE(line_with(*child, "probe.stop", seen));
  ASSERT_TRUE(line_with(*child, "probe.exit", seen));
  EXPECT_EQ(exit_of(*child), 0);
}

TEST(NodeProgram, ADrainStepNotDoneByTheDeadlineIsLeft) {
  Probe probe;
  auto child = probe.node().with({"--probe-drain-ms", "never", "--node-drain-ms", "300"}).start();
  std::vector<std::string> seen;
  ASSERT_TRUE(line_with(*child, "probe.started", seen));
  const auto signalled = Clock::now();
  child->signal(SIGTERM);
  ASSERT_TRUE(line_with(*child, "probe.drain", seen));
  ASSERT_TRUE(line_with(*child, "probe.stop", seen));
  ASSERT_TRUE(line_with(*child, "probe.exit", seen));
  EXPECT_EQ(exit_of(*child), 0);
  EXPECT_LT(Clock::now() - signalled, 15s);
}

TEST(NodeProgram, ASecondSignalStopsAtOnce) {
  Probe probe;
  auto child = probe.node().with({"--probe-drain-ms", "never", "--node-drain-ms", "600000"}).start();
  std::vector<std::string> seen;
  ASSERT_TRUE(line_with(*child, "probe.started", seen));
  child->signal(SIGTERM);
  ASSERT_TRUE(line_with(*child, "probe.drain", seen));
  child->signal(SIGINT);
  EXPECT_EQ(exit_of(*child), 1);
}

TEST(NodeProgram, NodeSecondsEndsTheRunAsASignalWould) {
  Probe probe;
  auto child = probe.node().with({"--node-seconds", "1"}).start();
  std::vector<std::string> seen;
  ASSERT_TRUE(line_with(*child, "probe.started", seen));
  ASSERT_TRUE(line_with(*child, "probe.drain", seen));
  ASSERT_TRUE(line_with(*child, "probe.exit", seen));
  EXPECT_EQ(exit_of(*child), 0);
}

}  // namespace
}  // namespace dannan::supervise
