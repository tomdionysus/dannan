//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The examples the guides quote, each run as a process (the maintainer's ruling, 2026-11-23): an
// example that no longer builds fails the build, and one that no longer does what it shows fails
// here. Each exits 0 only when what it shows happened.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan {
namespace {

using Clock = std::chrono::steady_clock;

// The example's exit code, its output kept for the failure message.
int run(const std::string& name, std::vector<std::string> arguments, std::string& output) {
  test::TempDir dir;
  std::vector<std::string> argv{std::string(DANNAN_EXAMPLES_DIR) + "/" + name};
  for (auto& argument : arguments) argv.push_back(argument == "STATE" ? (dir.path() / "state").string() : std::move(argument));
  test::ChildProcess child(argv, dir.path() / (name + ".log"));
  output = child.read_all(Clock::now() + std::chrono::seconds(120));
  return child.wait(Clock::now() + std::chrono::seconds(30)).value_or(-1);
}

class Examples : public ::testing::TestWithParam<std::pair<std::string, std::vector<std::string>>> {};

TEST_P(Examples, EachRunsAndSucceeds) {
  std::string output;
  EXPECT_EQ(run(GetParam().first, GetParam().second, output), 0) << output;
}

INSTANTIATE_TEST_SUITE_P(Guides, Examples,
                         ::testing::Values(std::make_pair(std::string("founding_three"), std::vector<std::string>{"STATE"}),
                                           std::make_pair(std::string("routes"), std::vector<std::string>{"STATE"}),
                                           std::make_pair(std::string("delivery"), std::vector<std::string>{"STATE"}),
                                           std::make_pair(std::string("settings"), std::vector<std::string>{"STATE"}),
                                           std::make_pair(std::string("deadlines"), std::vector<std::string>{}),
                                           std::make_pair(std::string("hosted_node"), std::vector<std::string>{"--node-seconds", "1"}),
                                           std::make_pair(std::string("simulated_test"), std::vector<std::string>{})),
                         [](const auto& example) { return example.param.first; });

}  // namespace
}  // namespace dannan
