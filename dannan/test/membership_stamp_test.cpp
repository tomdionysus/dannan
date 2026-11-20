//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A record change's stamp never goes back (TODO/2026-11-20-stack-plan.md, item 6; the
// special-roles audit's finding): peers keep only a strictly newer record, so a change stamped
// from a wall clock stepped back was ignored until the wall caught up. Every change, endpoint,
// capabilities, path class and candidates as adverts already did, is stamped past the last.

#include <gtest/gtest.h>

#include <chrono>

#include "dannan/membership/membership.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::membership {
namespace {

using namespace std::chrono_literals;

NodeRecord self_record() {
  NodeRecord out;
  out.id.bytes[0] = 1;
  out.host = "10.0.0.1";
  out.port = 7437;
  return out;
}

struct Fixture {
  test::SteppedTime steady;
  test::SteppedWall wall;
  Membership members{self_record(), 30s, {}, std::chrono::milliseconds::max(), &steady, &wall};

  uint64_t stamp() { return members.self().seen_unix_ms; }
};

TEST(MembershipStamp, AChangeAfterTheWallStepsBackIsStillNewer) {
  Fixture f;
  ASSERT_TRUE(f.members.set_capabilities(capability_relay));
  const auto first = f.stamp();
  f.wall.advance(-std::chrono::milliseconds(60'000));
  ASSERT_TRUE(f.members.set_path_class(PathClass::relay_only));
  EXPECT_GT(f.stamp(), first);
  const auto second = f.stamp();
  ASSERT_TRUE(f.members.set_inbound_capable(false));
  EXPECT_GT(f.stamp(), second);
  const auto third = f.stamp();
  ASSERT_TRUE(f.members.set_candidates({{"203.0.113.9", 40000, true, 2}}));
  EXPECT_GT(f.stamp(), third);
  const auto fourth = f.stamp();
  f.members.set_endpoint("10.0.0.9", 7438);
  EXPECT_GT(f.stamp(), fourth);
}

TEST(MembershipStamp, TwoChangesInOneMillisecondAreOrdered) {
  Fixture f;
  ASSERT_TRUE(f.members.set_capabilities(capability_relay));
  const auto first = f.stamp();
  ASSERT_TRUE(f.members.set_path_class(PathClass::relay_only));
  EXPECT_GT(f.stamp(), first);
}

TEST(MembershipStamp, AChangeIsNewerThanACopyHandedOutInTheSameMillisecond) {
  Fixture f;
  const auto copy = f.members.self();
  ASSERT_TRUE(f.members.set_capabilities(capability_relay));
  EXPECT_GT(f.stamp(), copy.seen_unix_ms);
}

}  // namespace
}  // namespace dannan::membership
