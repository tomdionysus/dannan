//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Who a node is: the inventory's "Identity, time, state files" rows. The
// durable node id created on first start, a fresh durability epoch per
// process, the state path locked before any state file is read.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <fstream>

#include "dannan/identity/node_identity.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::identity {
namespace {

ClusterKey key_in(const std::filesystem::path& dir) {
  std::ofstream(dir / "cluster.key") << "identity test";
  return load_cluster_key(dir / "cluster.key");
}

TEST(NodeIdentity, TheNodeIdIsCreatedOnceAndKept) {
  test::TempDir dir;
  const auto state = dir.path() / "state";
  const auto first = load_or_create_node_id(state);
  EXPECT_EQ(load_or_create_node_id(state), first);
  std::ifstream input(state / "node-id");
  std::string text;
  std::getline(input, text);
  EXPECT_EQ(text, to_string(first));
  struct stat info{};
  ASSERT_EQ(::stat((state / "node-id").c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600u);
}

TEST(NodeIdentity, ACorruptNodeIdRefusesToStart) {
  test::TempDir dir;
  std::ofstream(dir.path() / "node-id") << "not an id\n";
  EXPECT_THROW(load_or_create_node_id(dir.path()), std::runtime_error);
}

// Peers tell a restart from a reconnect by the epoch: fresh every process,
// while the id stays.
TEST(NodeIdentity, EachStartHasAFreshDurabilityEpoch) {
  test::TempDir dir;
  const NodeIdentity first(dir.path(), key_in(dir.path()));
  const NodeIdentity second(dir.path(), key_in(dir.path()));
  EXPECT_EQ(first.id, second.id);
  EXPECT_NE(first.durability_epoch, second.durability_epoch);
  EXPECT_NE(first.durability_epoch, NodeId{});
}

// One process per state path: a second lock is refused while the first is
// held, and granted once it is released.
TEST(StateLock, OneHolderAtATime) {
  test::TempDir dir;
  {
    StateLock held(dir.path());
    EXPECT_THROW(StateLock second(dir.path()), std::runtime_error);
  }
  EXPECT_NO_THROW(StateLock again(dir.path()));
  EXPECT_TRUE(std::filesystem::exists(dir.path() / ".dannan.lock"));
}

}  // namespace
}  // namespace dannan::identity
