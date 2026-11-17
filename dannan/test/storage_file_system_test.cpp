//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The file-system interface (TODO/2026-11-15-file-system-interface.md): one contract, kept by the
// POSIX file system and by the simulated disk alike; then what the simulated disk does to a crash;
// then durable_replace_file and durable_create_file on it, crashed at every step.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "dannan/identity/durable_file.h"
#include "dannan/identity/node_identity.h"
#include "dannan/storage/file_system.h"
#include "dannan/test/fixtures/sim/disk.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::storage {
namespace {

using test::sim::Disk;
using test::sim::Random;

std::span<const uint8_t> bytes(const std::string& text) { return {reinterpret_cast<const uint8_t*>(text.data()), text.size()}; }

// The whole of a file, or none if it cannot be read.
std::optional<std::string> contents(FileSystem& fs, const Path& path) {
  auto file = fs.open(path, OpenMode::reading());
  if (!file) return std::nullopt;
  const auto size = file.value()->size();
  if (!size) return std::nullopt;
  std::string out(size.value(), '\0');
  auto read = file.value()->read_at(0, std::span(reinterpret_cast<uint8_t*>(out.data()), out.size()));
  if (!read || read.value() != out.size()) return std::nullopt;
  return out;
}

void write(FileSystem& fs, const Path& path, const std::string& text) {
  auto file = std::move(fs.open(path, OpenMode::creating())).or_throw();
  ASSERT_TRUE(file->truncate(0));
  ASSERT_TRUE(file->write_at(0, bytes(text)));
}

// --- The contract, on both ---------------------------------------------------------------------

enum class Kind { posix, simulated };

class Contract : public ::testing::TestWithParam<Kind> {
 protected:
  Contract() {
    if (GetParam() == Kind::simulated) {
      _disk = std::make_unique<Disk>(_random);
      _root = "/state";
      (void)_disk->make_directories(_root);
    } else {
      _dir.emplace();
      _root = _dir->path();
    }
  }
  FileSystem& fs() { return _disk ? static_cast<FileSystem&>(*_disk) : posix_file_system(); }
  const Path& root() const { return _root; }

 private:
  Random _random{1};
  std::unique_ptr<Disk> _disk;
  std::optional<test::TempDir> _dir;
  Path _root;
};

TEST_P(Contract, WritesReadsAppendsAndTruncates) {
  auto file = std::move(fs().open(root() / "a", OpenMode::creating())).or_throw();
  EXPECT_EQ(file->write_at(0, bytes("hello")).value(), 5u);
  EXPECT_EQ(file->append(bytes(" world")).value(), 6u);
  EXPECT_EQ(file->size().value(), 11u);
  std::string half(4, '\0');
  EXPECT_EQ(file->read_at(7, std::span(reinterpret_cast<uint8_t*>(half.data()), half.size())).value(), 4u);
  EXPECT_EQ(half, "orld");
  // A read past the end is short.
  EXPECT_EQ(file->read_at(9, std::span(reinterpret_cast<uint8_t*>(half.data()), half.size())).value(), 2u);
  EXPECT_TRUE(file->truncate(5));
  EXPECT_TRUE(file->sync());
  EXPECT_TRUE(file->close());
  EXPECT_EQ(contents(fs(), root() / "a"), "hello");
}

TEST_P(Contract, MissingIsAbsentAndAnExclusiveCreateOfAnExistingFileIsExists) {
  const auto missing = fs().open(root() / "none", OpenMode::reading());
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorClass::absent);
  EXPECT_EQ(fs().open(root() / "no-dir" / "a", OpenMode::creating()).error().kind, ErrorClass::absent);
  EXPECT_EQ(fs().stat(root() / "none").error().kind, ErrorClass::absent);
  write(fs(), root() / "a", "x");
  const auto again = fs().open(root() / "a", OpenMode::creating(true));
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error().kind, ErrorClass::exists);
  EXPECT_EQ(again.error().path, root() / "a");
}

TEST_P(Contract, ARenameReplacesItsTargetAndALinkRefusesATakenName) {
  write(fs(), root() / "a", "new");
  write(fs(), root() / "b", "old");
  EXPECT_TRUE(fs().rename(root() / "a", root() / "b"));
  EXPECT_EQ(contents(fs(), root() / "b"), "new");
  EXPECT_EQ(fs().stat(root() / "a").error().kind, ErrorClass::absent);
  write(fs(), root() / "c", "c");
  const auto taken = fs().link(root() / "c", root() / "b");
  ASSERT_FALSE(taken);
  EXPECT_EQ(taken.error().kind, ErrorClass::exists);
  EXPECT_EQ(contents(fs(), root() / "b"), "new");
  EXPECT_TRUE(fs().link(root() / "c", root() / "d"));
  EXPECT_EQ(contents(fs(), root() / "d"), "c");
}

TEST_P(Contract, ListsEntriesSortedWithTypeAndSize) {
  write(fs(), root() / "b", "four");
  write(fs(), root() / "a", "");
  ASSERT_TRUE(fs().make_directories(root() / "c" / "d"));
  const auto listed = fs().list(root());
  ASSERT_TRUE(listed);
  ASSERT_EQ(listed.value().size(), 3u);
  EXPECT_EQ(listed.value()[0].name, "a");
  EXPECT_EQ(listed.value()[0].size, 0u);
  EXPECT_EQ(listed.value()[1].name, "b");
  EXPECT_EQ(listed.value()[1].type, EntryType::file);
  EXPECT_EQ(listed.value()[1].size, 4u);
  EXPECT_EQ(listed.value()[2].type, EntryType::directory);
  EXPECT_EQ(fs().stat(root() / "b").value().size, 4u);
  EXPECT_EQ(fs().list(root() / "none").error().kind, ErrorClass::absent);
}

TEST_P(Contract, MakesDirectoriesLeavesThosePresentAndRefusesAFileInTheWay) {
  ASSERT_TRUE(fs().make_directories(root() / "x" / "y"));
  ASSERT_TRUE(fs().make_directories(root() / "x" / "y"));
  EXPECT_EQ(fs().stat(root() / "x" / "y").value().type, EntryType::directory);
  write(fs(), root() / "f", "f");
  EXPECT_EQ(fs().make_directories(root() / "f" / "g").error().kind, ErrorClass::exists);
}

TEST_P(Contract, RemovesAFileOrAnEmptyDirectoryOnly) {
  write(fs(), root() / "a", "a");
  ASSERT_TRUE(fs().make_directories(root() / "d"));
  write(fs(), root() / "d" / "e", "e");
  EXPECT_EQ(fs().remove(root() / "d").error().kind, ErrorClass::exists);
  EXPECT_TRUE(fs().remove(root() / "d" / "e"));
  EXPECT_TRUE(fs().remove(root() / "d"));
  EXPECT_TRUE(fs().remove(root() / "a"));
  EXPECT_EQ(fs().remove(root() / "a").error().kind, ErrorClass::absent);
}

TEST_P(Contract, SyncsSucceedAndSpaceIsReported) {
  write(fs(), root() / "a", "a");
  EXPECT_TRUE(fs().sync_directory(root()));
  EXPECT_TRUE(fs().sync_all(root()));
  const auto space = fs().space(root());
  ASSERT_TRUE(space);
  EXPECT_GT(space.value().total, 0u);
  EXPECT_LE(space.value().available, space.value().total);
}

TEST_P(Contract, ALockIsHeldUntilItsHandleGoes) {
  auto first = fs().lock(root() / "lock");
  ASSERT_TRUE(first);
  const auto second = fs().lock(root() / "lock");
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().kind, ErrorClass::busy);
  first.value().reset();
  EXPECT_TRUE(fs().lock(root() / "lock"));
}

TEST_P(Contract, OrThrowRaisesTheErrorItCarries) {
  try {
    (void)std::move(fs().open(root() / "none", OpenMode::reading())).or_throw();
    FAIL() << "no throw";
  } catch (const StorageError& error) {
    EXPECT_EQ(error.error().kind, ErrorClass::absent);
    EXPECT_NE(std::string(error.what()).find("none"), std::string::npos) << error.what();
  }
}

TEST_P(Contract, DurableFilesReplaceAndCreateThroughIt) {
  identity::durable_replace_file(root() / "deep" / "state", "one", fs());
  identity::durable_replace_file(root() / "deep" / "state", "two", fs());
  EXPECT_EQ(contents(fs(), root() / "deep" / "state"), "two");
  identity::durable_create_file(root() / "deep" / "made", "once", fs());
  EXPECT_THROW(identity::durable_create_file(root() / "deep" / "made", "twice", fs()), std::filesystem::filesystem_error);
  EXPECT_EQ(contents(fs(), root() / "deep" / "made"), "once");
  // No temporary left behind.
  EXPECT_EQ(fs().list(root() / "deep").value().size(), 2u);
}

TEST_P(Contract, AStateLockRefusesASecondHolder) {
  identity::StateLock held(root(), ".lock", fs());
  EXPECT_THROW(identity::StateLock(root(), ".lock", fs()), std::runtime_error);
}

INSTANTIATE_TEST_SUITE_P(FileSystems, Contract, ::testing::Values(Kind::posix, Kind::simulated), [](const ::testing::TestParamInfo<Kind>& which) {
  return which.param == Kind::posix ? std::string("Posix") : std::string("Simulated");
});

// --- The simulated disk under a crash ----------------------------------------------------------

TEST(SimulatedDisk, ACrashKeepsWhatWasSyncedAndLosesWhatWasNot) {
  Random random(1);
  Disk disk(random);
  ASSERT_TRUE(disk.make_directories("/d"));
  ASSERT_TRUE(disk.sync_directory("/"));
  write(disk, "/d/kept", "kept");
  ASSERT_TRUE(disk.open("/d/kept", OpenMode::writing()).value()->sync());
  write(disk, "/d/lost", "lost");
  ASSERT_TRUE(disk.sync_directory("/d"));
  // Written after the sync: lost.
  ASSERT_TRUE(disk.open("/d/kept", OpenMode::writing()).value()->append(bytes("+more")));
  disk.crash();
  EXPECT_EQ(contents(disk, "/d/kept"), "kept");
  // Its name was synced and its data never: there, and empty.
  EXPECT_EQ(contents(disk, "/d/lost"), "");
}

TEST(SimulatedDisk, ANameIsLostUnlessItsDirectoryWasSynced) {
  Random random(1);
  Disk disk(random);
  write(disk, "/a", "a");
  ASSERT_TRUE(disk.open("/a", OpenMode::writing()).value()->sync());
  disk.crash();
  EXPECT_EQ(disk.stat("/a").error().kind, ErrorClass::absent);
  // A rename not synced is undone.
  write(disk, "/a", "a");
  write(disk, "/b", "b");
  ASSERT_TRUE(disk.sync_all("/"));
  ASSERT_TRUE(disk.rename("/a", "/b"));
  disk.crash();
  EXPECT_EQ(contents(disk, "/a"), "a");
  EXPECT_EQ(contents(disk, "/b"), "b");
}

TEST(SimulatedDisk, AFileInADirectoryWhoseEntryWasLostGoesWithIt) {
  Random random(1);
  Disk disk(random);
  ASSERT_TRUE(disk.make_directories("/new"));
  write(disk, "/new/a", "a");
  ASSERT_TRUE(disk.open("/new/a", OpenMode::writing()).value()->sync());
  ASSERT_TRUE(disk.sync_directory("/new"));
  // "/" never synced: "/new" is lost, and "/new/a" with it.
  disk.crash();
  EXPECT_EQ(disk.stat("/new").error().kind, ErrorClass::absent);
  EXPECT_EQ(disk.stat("/new/a").error().kind, ErrorClass::absent);
}

TEST(SimulatedDisk, ALyingSyncKeepsNothing) {
  Random random(1);
  Disk disk(random);
  write(disk, "/a", "a");
  ASSERT_TRUE(disk.sync_all("/"));
  disk.lie_on_syncs(true);
  ASSERT_TRUE(disk.open("/a", OpenMode::writing()).value()->append(bytes("b")));
  EXPECT_TRUE(disk.sync_all("/"));
  disk.crash();
  EXPECT_EQ(contents(disk, "/a"), "a");
}

// A failed sync is io, and the dirty data is then sometimes kept and sometimes dropped (Linux's
// behaviour): the caller cannot know which.
TEST(SimulatedDisk, AFailedSyncIsIoAndMayDropTheDirtyData) {
  size_t kept = 0, dropped = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    Random random(seed);
    Disk disk(random);
    write(disk, "/a", "a");
    ASSERT_TRUE(disk.sync_all("/"));
    disk.fail_syncs_one_in(1);
    auto file = disk.open("/a", OpenMode::writing()).value();
    ASSERT_TRUE(file->append(bytes("b")));
    const auto synced = file->sync();
    ASSERT_FALSE(synced);
    EXPECT_EQ(synced.error().kind, ErrorClass::io);
    ++(contents(disk, "/a") == "ab" ? kept : dropped);
  }
  EXPECT_GT(kept, 0u);
  EXPECT_GT(dropped, 0u);
}

// A torn crash keeps a prefix of what a file grew by since its sync, of a length the seed draws.
TEST(SimulatedDisk, ATornCrashKeepsAPrefixOfTheUnsyncedGrowth) {
  std::set<std::string> seen;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    Random random(seed);
    Disk disk(random);
    write(disk, "/log", "head:");
    ASSERT_TRUE(disk.sync_all("/"));
    ASSERT_TRUE(disk.open("/log", OpenMode::writing()).value()->append(bytes("0123456789")));
    disk.crash(true);
    const auto left = contents(disk, "/log");
    ASSERT_TRUE(left);
    EXPECT_EQ(left->rfind("head:", 0), 0u);
    EXPECT_EQ(std::string("head:0123456789").rfind(*left, 0), 0u) << *left;
    seen.insert(*left);
  }
  EXPECT_GT(seen.size(), 2u);
}

TEST(SimulatedDisk, AFullDiskRefusesAsNoRoom) {
  Random random(1);
  Disk disk(random, 8);
  auto file = disk.open("/a", OpenMode::creating()).value();
  EXPECT_TRUE(file->write_at(0, bytes("12345678")));
  const auto over = file->append(bytes("9"));
  ASSERT_FALSE(over);
  EXPECT_EQ(over.error().kind, ErrorClass::no_room);
  EXPECT_EQ(disk.space("/").value().available, 0u);
}

// The power cut at an operation: that one and every later one fail until the crash, after which
// handles and locks from before are gone.
TEST(SimulatedDisk, ACutDiskFailsEverythingUntilItCrashes) {
  Random random(1);
  Disk disk(random);
  auto file = disk.open("/a", OpenMode::creating()).value();
  auto held = disk.lock("/lock");
  ASSERT_TRUE(held);
  disk.cut_after(1);
  EXPECT_TRUE(file->write_at(0, bytes("a")));
  EXPECT_EQ(file->write_at(1, bytes("b")).error().kind, ErrorClass::io);
  EXPECT_TRUE(disk.is_cut());
  EXPECT_EQ(disk.open("/a", OpenMode::reading()).error().kind, ErrorClass::io);
  disk.crash();
  EXPECT_FALSE(disk.is_cut());
  EXPECT_EQ(file->size().error().kind, ErrorClass::io);
  EXPECT_TRUE(disk.lock("/lock"));
}

// --- Durable files on it, crashed at every step ------------------------------------------------

// A replace cut at each of its operations in turn and crashed, torn or not: the file holds the old
// contents or the new, never neither, never a mixture; and once the replace returned, the new.
TEST(DurableFileOnTheDisk, AReplaceCrashedAtAnyStepLeavesTheOldOrTheNew) {
  for (const bool tear : {false, true})
    for (uint64_t at = 0;; ++at) {
      Random random(at + 1);
      Disk disk(random);
      ASSERT_TRUE(disk.make_directories("/state"));
      ASSERT_TRUE(disk.sync_directory("/"));
      identity::durable_replace_file("/state/file", "the old contents", disk);
      const auto before = disk.operations();
      disk.cut_after(at);
      bool returned = true;
      try {
        identity::durable_replace_file("/state/file", "the new contents, longer", disk);
      } catch (const std::runtime_error&) {
        returned = false;
      }
      const bool reached_the_end = disk.operations() - before < at;
      disk.crash(tear);
      const auto left = contents(disk, "/state/file");
      SCOPED_TRACE("cut after " + std::to_string(at) + (tear ? ", torn" : ""));
      ASSERT_TRUE(left == "the old contents" || left == "the new contents, longer") << (left ? *left : "(none)");
      if (returned) {
        EXPECT_EQ(left, "the new contents, longer");
      }
      if (reached_the_end) break;
    }
}

// The same for a file whose directories the replace makes: cut at each step, the file is absent or
// whole; once the replace returned, a crash keeps it (each directory it made synced into its parent).
TEST(DurableFileOnTheDisk, AReplaceThatMadeItsDirectoriesSurvivesACrashOnceReturned) {
  for (uint64_t at = 0;; ++at) {
    Random random(at + 1);
    Disk disk(random);
    disk.cut_after(at);
    bool returned = true;
    try {
      identity::durable_replace_file("/state/deep/file", "contents", disk);
    } catch (const std::runtime_error&) {
      returned = false;
    }
    const bool reached_the_end = disk.operations() < at;
    disk.crash();
    const auto left = contents(disk, "/state/deep/file");
    SCOPED_TRACE("cut after " + std::to_string(at));
    EXPECT_TRUE(!left || *left == "contents") << *left;
    if (returned) {
      EXPECT_EQ(left, "contents");
    }
    if (reached_the_end) break;
  }
}

// The contract's word on a lying sync: a replace that returned can be undone by a crash, and only
// the caller's own checks (seals, checksums) can tell.
TEST(DurableFileOnTheDisk, ALyingSyncCanUndoAReplaceThatReturned) {
  Random random(1);
  Disk disk(random);
  ASSERT_TRUE(disk.make_directories("/state"));
  ASSERT_TRUE(disk.sync_directory("/"));
  identity::durable_replace_file("/state/file", "old", disk);
  disk.lie_on_syncs(true);
  identity::durable_replace_file("/state/file", "new", disk);
  disk.crash();
  const auto left = contents(disk, "/state/file");
  EXPECT_NE(left, "new");
}

// Two creators: exactly one wins, and the winner's contents survive a crash once it returned.
TEST(DurableFileOnTheDisk, ACreateRefusesASecondAndSurvivesACrash) {
  Random random(1);
  Disk disk(random);
  ASSERT_TRUE(disk.make_directories("/state"));
  ASSERT_TRUE(disk.sync_directory("/"));
  identity::durable_create_file("/state/key", "first", disk);
  EXPECT_THROW(identity::durable_create_file("/state/key", "second", disk), std::filesystem::filesystem_error);
  disk.crash();
  EXPECT_EQ(contents(disk, "/state/key"), "first");
}

}  // namespace
}  // namespace dannan::storage
