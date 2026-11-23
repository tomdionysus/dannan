//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The process runner (item 23; Louhi's and Macha's): a fixed argument list and environment, no
// shell, output bounded or to a file, the whole process group killed at the deadline, on cancel,
// on stop and when its leader exits, a nice value, and one posted completion.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "dannan/supervise/command.h"

namespace dannan::supervise {
namespace {

using namespace std::chrono_literals;

struct Runner {
  boost::asio::io_context io;
  ProcessCommands commands{io};

  // Runs one command to its answer: the io context runs until nothing is left.
  CommandOutcome run(Invocation invocation, contract::Deadline deadline = 10s) {
    std::optional<CommandOutcome> out;
    (void)commands.run(std::move(invocation), deadline, io.get_executor(), [&](CommandOutcome outcome) { out = std::move(outcome); });
    io.restart();
    io.run();
    EXPECT_TRUE(out);
    return out.value_or(CommandOutcome{});
  }
};

// Field by field: GCC's -Wmissing-field-initializers refuses a braced one that leaves any out.
Invocation invocation(std::vector<std::string> argv, std::string input = {}) {
  Invocation out;
  out.argv = std::move(argv);
  out.input = std::move(input);
  return out;
}

Invocation shell(const std::string& script, std::string input = {}) { return invocation({"sh", "-c", script}, std::move(input)); }

// A process gone, or a zombie no one has reaped yet: either way it runs no more. Bounded: a
// SIGKILL takes effect when the process is next scheduled.
bool dead(pid_t pid) {
  for (int i = 0; i < 500; ++i) {
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    if (!stat) return true;
    std::string line;
    std::getline(stat, line);
    const auto close = line.rfind(')');
    if (close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z') return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

TEST(Command, ItsOutputAndExitCode) {
  Runner runner;
  const auto out = runner.run(shell("echo out; echo err >&2; exit 3"));
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_EQ(out.value.exit_code, 3);
  EXPECT_EQ(out.value.output, "out\nerr\n");
  EXPECT_FALSE(out.value.truncated);
}

TEST(Command, ItsInputIsWrittenToItsStdin) {
  Runner runner;
  const auto out = runner.run(invocation({"cat"}, "hello\nthere"));
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_EQ(out.value.output, "hello\nthere");
}

TEST(Command, OneKilledBySignalExitsWith128AndTheSignal) {
  Runner runner;
  const auto out = runner.run(shell("kill -9 $$"));
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_EQ(out.value.exit_code, 128 + 9);
}

TEST(Command, AProgramNotFoundIsUnavailableAndNamed) {
  Runner runner;
  const auto out = runner.run(invocation({"no-such-program-dannan"}));
  EXPECT_EQ(out.fault, contract::Fault::unavailable);
  EXPECT_NE(out.message.find("no-such-program-dannan"), std::string::npos) << out.message;
  // A path is taken as it is, never searched.
  EXPECT_EQ(runner.run(invocation({"./sh"})).fault, contract::Fault::unavailable);
}

TEST(Command, AnEmptyArgumentListOrABadNiceIsRefused) {
  Runner runner;
  const auto ignore = [](CommandOutcome) {};
  EXPECT_THROW(runner.commands.run(Invocation{}, 1s, runner.io.get_executor(), ignore), std::invalid_argument);
  EXPECT_THROW(runner.commands.run(invocation({""}), 1s, runner.io.get_executor(), ignore), std::invalid_argument);
  auto negative = shell("true");
  negative.nice = -1;
  EXPECT_THROW(runner.commands.run(negative, 1s, runner.io.get_executor(), ignore), std::invalid_argument);
  auto high = shell("true");
  high.nice = 20;
  EXPECT_THROW(runner.commands.run(high, 1s, runner.io.get_executor(), ignore), std::invalid_argument);
}

TEST(Command, ItsOutputIsBoundedAndTheCutSaid) {
  Runner runner;
  auto invocation = shell("yes | head -c 100000");
  invocation.max_output = 1000;
  const auto out = runner.run(invocation);
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_EQ(out.value.exit_code, 0);
  EXPECT_EQ(out.value.output.size(), 1000u);
  EXPECT_TRUE(out.value.truncated);
}

TEST(Command, ItsEnvironmentIsThePathAndTheLocaleOnly) {
  Runner runner;
  const auto out = runner.run(invocation({"env"}));
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_EQ(out.value.output, "PATH=/usr/sbin:/usr/bin:/sbin:/bin\nLC_ALL=C\n");
}

TEST(Command, ADeadlineKillsTheWholeGroup) {
  Runner runner;
  const auto started = std::chrono::steady_clock::now();
  const auto out = runner.run(shell("sleep 30 & echo $!; wait"), 300ms);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
  EXPECT_EQ(out.fault, contract::Fault::timed_out);
  // Its output so far, as evidence: the background child's pid.
  const auto child = std::stoi(out.value.output);
  EXPECT_TRUE(dead(child));
}

// A leader that leaves a child holding its output: the child is killed as the leader exits, so the
// run ends then, not at its deadline.
TEST(Command, ALeadersExitKillsWhatItLeft) {
  Runner runner;
  const auto started = std::chrono::steady_clock::now();
  const auto out = runner.run(shell("sleep 30 & echo $!"), 20s);
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
  EXPECT_TRUE(dead(std::stoi(out.value.output)));
}

TEST(Command, ACancelKillsItAndFailsItCancelled) {
  Runner runner;
  std::optional<CommandOutcome> out;
  const auto id = runner.commands.run(invocation({"sleep", "30"}), 20s, runner.io.get_executor(), [&](CommandOutcome outcome) { out = outcome; });
  runner.commands.cancel(id);
  runner.commands.cancel(id + 1000);
  runner.io.run();
  ASSERT_TRUE(out);
  EXPECT_EQ(out->fault, contract::Fault::cancelled);
}

TEST(Command, AStopCancelsEveryRunAndEveryLaterOne) {
  Runner runner;
  std::vector<CommandOutcome> outs;
  for (int i = 0; i < 2; ++i)
    (void)runner.commands.run(invocation({"sleep", "30"}), 20s, runner.io.get_executor(), [&](CommandOutcome outcome) { outs.push_back(outcome); });
  runner.commands.stop();
  (void)runner.commands.run(invocation({"true"}), 20s, runner.io.get_executor(), [&](CommandOutcome outcome) { outs.push_back(outcome); });
  runner.io.run();
  ASSERT_EQ(outs.size(), 3u);
  for (const auto& out : outs) EXPECT_EQ(out.fault, contract::Fault::cancelled) << out.message;
}

TEST(Command, ItsOutputMayGoToAFile) {
  Runner runner;
  const auto file = std::filesystem::path(::testing::TempDir()) / "dannan-command-output.txt";
  std::filesystem::remove(file);
  auto invocation = shell("echo to-file; echo and-err >&2");
  invocation.output_file = file;
  const auto out = runner.run(invocation);
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_TRUE(out.value.output.empty());
  std::stringstream text;
  text << std::ifstream(file).rdbuf();
  EXPECT_EQ(text.str(), "to-file\nand-err\n");
  EXPECT_EQ(std::filesystem::status(file).permissions() & std::filesystem::perms::all,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  std::filesystem::remove(file);
}

#ifdef __linux__
// Set before its input is written, so the shell that waits on its input runs at it.
TEST(Command, ItRunsAtItsNiceValue) {
  Runner runner;
  auto invocation = shell("read x; cut -d' ' -f19 /proc/$$/stat", "go\n");
  invocation.nice = 7;
  const auto out = runner.run(invocation);
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_EQ(out.value.output, "7\n");
}

// A descriptor this process holds, even one not marked close-on-exec, never reaches the child.
TEST(Command, NoDescriptorOfThisProcessReachesTheChild) {
  Runner runner;
  const int held = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(held, 3);
  const auto out = runner.run(shell("test -e /proc/$$/fd/" + std::to_string(held) + " && echo leaked || echo closed"));
  ::close(held);
  ASSERT_TRUE(out.ok()) << out.message;
  EXPECT_EQ(out.value.output, "closed\n");
}
#endif

}  // namespace
}  // namespace dannan::supervise
