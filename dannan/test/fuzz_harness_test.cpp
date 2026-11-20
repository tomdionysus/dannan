//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The exported fuzz harness (TODO/2026-11-20-stack-plan.md, item 17; Kvasir's and Mimir's ask):
// a hosted system registers its targets and runs them in its own test binary through
// dannan::testing, with no clang, and builds a fuzz binary from two lines. Every draw comes from
// SplitMix64, so a seed gives the same inputs on libc++ and libstdc++.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dannan/fuzz/fuzz.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::fuzz {
namespace {

Target recording(std::vector<net::Bytes>& seen, std::vector<net::Bytes> seeds) {
  return Target{"recording", [&seen](std::span<const uint8_t> data) { seen.emplace_back(data.begin(), data.end()); }, [seeds] { return seeds; }};
}

TEST(FuzzHarness, ASmokeRunsTheSeedsThenMutationsThenNoiseThenTheEmptyInput) {
  std::vector<net::Bytes> seen;
  const auto target = recording(seen, {{1, 2, 3}, {4, 5}});
  SmokeOptions options;
  options.mutations = 10;
  options.noise = 4;
  const auto ran = smoke(target, options);
  EXPECT_EQ(ran, 2u + 10u + 4u + 1u);
  ASSERT_EQ(seen.size(), ran);
  EXPECT_EQ(seen[0], (net::Bytes{1, 2, 3}));
  EXPECT_EQ(seen[1], (net::Bytes{4, 5}));
  EXPECT_TRUE(seen.back().empty());
  for (size_t i = 2 + 10; i < 2 + 10 + 4; ++i) EXPECT_LE(seen[i].size(), options.max_noise);
}

TEST(FuzzHarness, ASeedGivesTheSameInputsEveryRun) {
  std::vector<net::Bytes> first, second;
  SmokeOptions options;
  options.mutations = 50;
  options.noise = 20;
  smoke(recording(first, {{9, 9, 9, 9, 9, 9}}), options);
  smoke(recording(second, {{9, 9, 9, 9, 9, 9}}), options);
  EXPECT_EQ(first, second);
  options.seed ^= 1;
  std::vector<net::Bytes> other;
  smoke(recording(other, {{9, 9, 9, 9, 9, 9}}), options);
  EXPECT_NE(first, other);
}

TEST(FuzzHarness, MutationChangesTheInput) {
  uint64_t state = 7;
  const net::Bytes seed{10, 20, 30, 40, 50, 60, 70, 80};
  size_t changed = 0;
  for (int i = 0; i < 100; ++i)
    if (mutate(seed, state) != seed) ++changed;
  EXPECT_GT(changed, 90u);
}

TEST(FuzzHarness, MutationOfAnEmptyInputDoesNotFail) {
  uint64_t state = 3;
  for (int i = 0; i < 200; ++i) (void)mutate({}, state);
}

TEST(FuzzHarness, ATargetThatThrowsWhatItDoesNotDocumentFailsTheSmoke) {
  const Target bad{"bad",
                   [](std::span<const uint8_t> data) {
                     if (data.size() > 2) throw std::logic_error("an undocumented throw");
                   },
                   [] { return std::vector<net::Bytes>{{1, 2, 3}}; }};
  EXPECT_THROW(smoke(bad), std::logic_error);
}

TEST(FuzzHarness, ATargetWithNoSeedsIsRefused) {
  const Target none{"none", [](std::span<const uint8_t>) {}, [] { return std::vector<net::Bytes>{}; }};
  EXPECT_THROW(smoke(none), std::invalid_argument);
}

TEST(FuzzHarness, ReplayRunsEveryFileAndEveryFileInADirectory) {
  test::TempDir dir;
  const auto one = dir.path() / "one.bin";
  const auto corpus = dir.path() / "corpus";
  std::filesystem::create_directories(corpus);
  std::ofstream(one, std::ios::binary) << "a";
  std::ofstream(corpus / "x", std::ios::binary) << "bc";
  std::ofstream(corpus / "y", std::ios::binary) << "def";
  std::vector<net::Bytes> seen;
  const std::vector<Target> targets{recording(seen, {{0}})};
  std::string one_arg = one.string(), corpus_arg = corpus.string();
  char program[] = "fuzz";
  char* argv[] = {program, one_arg.data(), corpus_arg.data()};
  EXPECT_EQ(replay(targets, 3, argv), 3u);
  EXPECT_EQ(seen.size(), 3u);
}

TEST(FuzzHarness, ReplayWithNoArgumentsRunsTheSeeds) {
  std::vector<net::Bytes> seen;
  const std::vector<Target> targets{recording(seen, {{1}, {2}})};
  char program[] = "fuzz";
  char* argv[] = {program};
  EXPECT_EQ(replay(targets, 1, argv), 2u);
}

TEST(FuzzHarness, RunAllHandsTheBytesToEveryTarget) {
  std::vector<net::Bytes> a, b;
  const std::vector<Target> targets{recording(a, {{0}}), recording(b, {{0}})};
  const net::Bytes input{5, 6};
  run_all(targets, input);
  EXPECT_EQ(a, std::vector<net::Bytes>{input});
  EXPECT_EQ(b, std::vector<net::Bytes>{input});
}

}  // namespace
}  // namespace dannan::fuzz
