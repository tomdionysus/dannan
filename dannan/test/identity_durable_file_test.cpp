//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Durable file replacement: the inventory's row "durable file replacement
// (write, sync, rename)". A reader sees the old contents or the new, never a
// mixture, and a failure leaves the old contents and no debris.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <vector>

#include "dannan/identity/durable_file.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::identity {
namespace {

std::string read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

size_t entries(const std::filesystem::path& directory) {
  size_t count = 0;
  for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(directory)) ++count;
  return count;
}

TEST(DurableFile, WritesANewFileAndItsDirectories) {
  test::TempDir dir;
  const auto path = dir.path() / "state" / "node-id";
  durable_replace_file(path, "abc");
  EXPECT_EQ(read(path), "abc");
  EXPECT_EQ(entries(path.parent_path()), 1u);
}

TEST(DurableFile, ReplacesTheWholeFile) {
  test::TempDir dir;
  const auto path = dir.path() / "roster";
  durable_replace_file(path, "a much longer first version");
  durable_replace_file(path, "short");
  EXPECT_EQ(read(path), "short");
  EXPECT_EQ(entries(dir.path()), 1u);
}

TEST(DurableFile, OnlyItsOwnerMayReadIt) {
  test::TempDir dir;
  const auto path = dir.path() / "key";
  durable_replace_file(path, "secret");
  const auto permissions = std::filesystem::status(path).permissions();
  EXPECT_EQ(permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all), std::filesystem::perms::none);
}

// The rename cannot replace a directory: the call fails, the target is as it
// was, and no temporary is left beside it.
TEST(DurableFile, AFailureLeavesTheOldStateAndNoDebris) {
  test::TempDir dir;
  const auto path = dir.path() / "occupied";
  std::filesystem::create_directory(path);
  EXPECT_THROW(durable_replace_file(path, "x"), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_directory(path));
  EXPECT_EQ(entries(dir.path()), 1u);
}

// Made once and never replaced: an existing file is refused, distinguishably,
// and left as it was; no temporary is left behind.
TEST(DurableCreateFile, NeverReplacesAndSaysSo) {
  test::TempDir dir;
  const auto path = dir.path() / "sub" / "keyring";
  durable_create_file(path, "first");
  try {
    durable_create_file(path, "second");
    FAIL();
  } catch (const std::filesystem::filesystem_error& error) {
    EXPECT_EQ(error.code(), std::errc::file_exists);
  }
  std::ifstream input(path);
  EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()), "first");
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(path.parent_path()), std::filesystem::directory_iterator()), 1);
  EXPECT_EQ(std::filesystem::status(path).permissions() & std::filesystem::perms::all,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}

// Of creators racing on one name, exactly one wins.
TEST(DurableCreateFile, OfRacingCreatorsExactlyOneWins) {
  test::TempDir dir;
  const auto path = dir.path() / "once";
  std::atomic<int> won{0}, refused{0};
  std::vector<std::thread> racers;
  for (int i = 0; i < 8; ++i)
    racers.emplace_back([&, i] {
      try {
        durable_create_file(path, "racer " + std::to_string(i));
        ++won;
      } catch (const std::filesystem::filesystem_error&) {
        ++refused;
      }
    });
  for (auto& racer : racers) racer.join();
  EXPECT_EQ(won.load(), 1);
  EXPECT_EQ(refused.load(), 7);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(dir.path()), std::filesystem::directory_iterator()), 1);
}

// The directories a write makes are owner-only, like the file; one that
// was there is left as it was.
TEST(DurableFile, MakesItsDirectoriesOwnerOnly) {
  test::TempDir dir;
  std::filesystem::create_directories(dir.path() / "kept");
  std::filesystem::permissions(dir.path() / "kept",
                               std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);
  durable_create_file(dir.path() / "kept" / "a" / "b" / "custody", "x");
  const auto all = std::filesystem::perms::all;
  EXPECT_EQ(std::filesystem::status(dir.path() / "kept" / "a").permissions() & all, std::filesystem::perms::owner_all);
  EXPECT_EQ(std::filesystem::status(dir.path() / "kept" / "a" / "b").permissions() & all, std::filesystem::perms::owner_all);
  EXPECT_EQ(std::filesystem::status(dir.path() / "kept").permissions() & all,
            std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec);
}

}  // namespace
}  // namespace dannan::identity
