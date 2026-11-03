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

#include <filesystem>
#include <fstream>
#include <sstream>

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

}  // namespace
}  // namespace dannan::identity
