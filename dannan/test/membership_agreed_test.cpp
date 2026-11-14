//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The roster as last agreed, and the partition reading over it (P2, Tom,
// 2026-11-13): a join adds a node, only an operator's retire takes one out,
// never absence; an even split is a minority. Persisted (DNNAGR01).

#include <gtest/gtest.h>

#include <fstream>

#include "dannan/membership/agreed_roster.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::membership {
namespace {

identity::NodeId id(uint8_t n) {
  identity::NodeId out;
  out.bytes[0] = n;
  return out;
}

// Self is always a member; a join adds once; what is held survives a reopen.
TEST(AgreedRoster, KeepsItsMembersAcrossARestart) {
  test::TempDir dir;
  const auto path = dir.path() / "agreed.bin";
  {
    AgreedRoster agreed(id(1), path);
    EXPECT_FALSE(agreed.found());
    EXPECT_EQ(agreed.members(), (std::set<identity::NodeId>{id(1)}));
    EXPECT_TRUE(agreed.join({id(2), id(3)}));
    EXPECT_FALSE(agreed.join({id(2)}));
    agreed.flush();
  }
  AgreedRoster again(id(1), path);
  EXPECT_TRUE(again.found());
  EXPECT_EQ(again.members(), (std::set<identity::NodeId>{id(1), id(2), id(3)}));
}

// Only a retire takes a node out, and it is written at once; this node
// cannot retire itself.
TEST(AgreedRoster, OnlyARetireTakesANodeOut) {
  test::TempDir dir;
  const auto path = dir.path() / "agreed.bin";
  AgreedRoster agreed(id(1), path);
  agreed.join({id(2), id(3)});
  EXPECT_TRUE(agreed.retire(id(3)));
  EXPECT_FALSE(agreed.retire(id(3)));
  EXPECT_THROW(agreed.retire(id(1)), std::invalid_argument);
  AgreedRoster reopened(id(1), path);
  EXPECT_EQ(reopened.members(), (std::set<identity::NodeId>{id(1), id(2)}));
}

// A file that is not an agreed roster is refused, not read as an empty one:
// that would shrink the denominator and could make a minority a majority.
TEST(AgreedRoster, RefusesAFileItCannotRead) {
  test::TempDir dir;
  const auto path = dir.path() / "agreed.bin";
  {
    std::ofstream out(path, std::ios::binary);
    out << "not an agreed roster";
  }
  EXPECT_THROW(AgreedRoster(id(1), path), std::runtime_error);
}

// The side: a majority only with more than half the agreed roster reachable,
// self counted; an even split is a minority (Tom, 2026-11-13); a node active
// but not agreed adds nothing until it joins.
TEST(Partition, IsAMajorityOnlyWithMoreThanHalf) {
  const std::set<identity::NodeId> three{id(1), id(2), id(3)};
  auto side = read_partition(three, {id(1), id(2)}, 5);
  EXPECT_TRUE(side.majority);
  EXPECT_EQ(side.members, 3u);
  EXPECT_EQ(side.reachable, 2u);
  EXPECT_EQ(side.unreachable, (std::vector<identity::NodeId>{id(3)}));
  EXPECT_EQ(side.unix_ms, 5u);
  EXPECT_FALSE(read_partition(three, {id(1)}, 5).majority);
  const std::set<identity::NodeId> two{id(1), id(2)};
  EXPECT_FALSE(read_partition(two, {id(1)}, 5).majority) << "an even split is a minority";
  EXPECT_TRUE(read_partition(two, {id(1), id(2)}, 5).majority);
  EXPECT_TRUE(read_partition({id(1)}, {id(1)}, 5).majority) << "a cluster of one";
  EXPECT_EQ(read_partition(two, {id(1), id(9)}, 5).reachable, 1u) << "active but not agreed";
}

}  // namespace
}  // namespace dannan::membership
