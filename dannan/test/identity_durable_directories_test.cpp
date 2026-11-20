//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Durable directories (TODO/2026-11-20-stack-plan.md, item 19; Macha's gap before it deletes its
// durable_file): a directory made is durable only once its parent is synced, so each one made is
// synced into its parent, deepest first; and the mode of made directories is the caller's, since
// Macha's live paths keep the umask's.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "dannan/identity/durable_file.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::identity {
namespace {

// The real file system, with every directory sync recorded.
class Recording final : public storage::FileSystem {
 public:
  storage::Result<std::unique_ptr<storage::File>> open(const storage::Path& p, storage::OpenMode m) override { return real().open(p, m); }
  storage::Result<void> rename(const storage::Path& a, const storage::Path& b) override { return real().rename(a, b); }
  storage::Result<void> link(const storage::Path& a, const storage::Path& b) override { return real().link(a, b); }
  storage::Result<void> remove(const storage::Path& p) override { return real().remove(p); }
  storage::Result<std::vector<storage::Entry>> list(const storage::Path& p) override { return real().list(p); }
  storage::Result<storage::Entry> stat(const storage::Path& p) override { return real().stat(p); }
  storage::Result<void> make_directories(const storage::Path& p, unsigned permissions) override { return real().make_directories(p, permissions); }
  storage::Result<void> sync_directory(const storage::Path& p) override {
    std::lock_guard lock(mutex);
    synced.push_back(p);
    return real().sync_directory(p);
  }
  storage::Result<void> sync_all(const storage::Path& p) override { return real().sync_all(p); }
  storage::Result<storage::Space> space(const storage::Path& p) override { return real().space(p); }
  storage::Result<std::unique_ptr<storage::Lock>> lock(const storage::Path& p) override { return real().lock(p); }

  std::mutex mutex;
  std::vector<storage::Path> synced;

 private:
  static storage::FileSystem& real() { return storage::posix_file_system(); }
};

std::filesystem::perms mode_of(const std::filesystem::path& p) { return std::filesystem::status(p).permissions() & std::filesystem::perms::all; }

TEST(DurableCreateDirectories, SyncsEachMadeDirectoryIntoItsParentDeepestFirst) {
  test::TempDir dir;
  Recording fs;
  const auto deepest = dir.path() / "a" / "b" / "c";
  durable_create_directories(deepest, 0700, fs);
  EXPECT_TRUE(std::filesystem::is_directory(deepest));
  EXPECT_EQ(fs.synced, (std::vector<storage::Path>{dir.path() / "a" / "b", dir.path() / "a", dir.path()}));
}

TEST(DurableCreateDirectories, DirectoriesAlreadyThereNeedNoSync) {
  test::TempDir dir;
  Recording fs;
  std::filesystem::create_directories(dir.path() / "x" / "y");
  durable_create_directories(dir.path() / "x" / "y", 0700, fs);
  EXPECT_TRUE(fs.synced.empty());
  durable_create_directories(dir.path() / "x" / "y" / "z", 0700, fs);
  EXPECT_EQ(fs.synced, (std::vector<storage::Path>{dir.path() / "x" / "y"}));
}

TEST(DurableCreateDirectories, MakesThemWithTheModeGiven) {
  test::TempDir dir;
  durable_create_directories(dir.path() / "shared" / "spool", 0750);
  const auto expected = std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec;
  EXPECT_EQ(mode_of(dir.path() / "shared"), expected);
  EXPECT_EQ(mode_of(dir.path() / "shared" / "spool"), expected);
}

TEST(DurableCreateDirectories, FailsUnderAFile) {
  test::TempDir dir;
  std::ofstream(dir.path() / "file") << "x";
  EXPECT_THROW(durable_create_directories(dir.path() / "file" / "sub"), std::runtime_error);
}

TEST(DurableSyncDirectory, SyncsTheOneDirectory) {
  test::TempDir dir;
  Recording fs;
  durable_sync_directory(dir.path(), fs);
  EXPECT_EQ(fs.synced, (std::vector<storage::Path>{dir.path()}));
}

TEST(DurableSyncDirectory, FailsForADirectoryThatIsNotThere) {
  test::TempDir dir;
  EXPECT_THROW(durable_sync_directory(dir.path() / "absent"), std::runtime_error);
}

TEST(DurableFile, MakesItsDirectoriesWithTheModeGivenAndTheFileOwnerOnly) {
  test::TempDir dir;
  const auto path = dir.path() / "lib" / "macha" / "hints";
  durable_replace_file(path, "x", storage::posix_file_system(), 0750);
  const auto group = std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::group_exec;
  EXPECT_EQ(mode_of(dir.path() / "lib"), group);
  EXPECT_EQ(mode_of(dir.path() / "lib" / "macha"), group);
  EXPECT_EQ(mode_of(path), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  durable_create_file(dir.path() / "lib" / "other" / "once", "y", storage::posix_file_system(), 0750);
  EXPECT_EQ(mode_of(dir.path() / "lib" / "other"), group);
}

}  // namespace
}  // namespace dannan::identity
