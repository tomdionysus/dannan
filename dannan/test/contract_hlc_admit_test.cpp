//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The clock rules at the stamp (TODO/2026-11-20-stack-plan.md, item 6; Dagda's D6, Mimir's
// Mi-2): admit() is the one check every receiver makes, and a stamp it refuses is never applied;
// a node out of bounds stops issuing at once, never waits; max_ahead is five seconds.

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>

#include "dannan/contract/hlc.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::contract {
namespace {

using namespace std::chrono_literals;

class MemoryCeiling final : public HlcCeiling {
 public:
  uint64_t load() override { return held; }
  void raise(uint64_t unix_ms) override { held = unix_ms; }
  uint64_t held = 0;
};

struct Fixture {
  test::SteppedWall wall;
  MemoryCeiling ceiling;
  HybridClock clock{wall, ceiling};

  HlcStamp ahead_by(std::chrono::milliseconds by) const { return HlcStamp::of(wall.unix_ms() + static_cast<uint64_t>(by.count()), 0); }
};

TEST(HlcAdmit, TheDefaultBoundIsFiveSeconds) {
  Fixture f;
  EXPECT_EQ(f.clock.max_ahead(), 5s);
}

TEST(HlcAdmit, AStampWithinTheBoundIsAdmittedAndMerged) {
  Fixture f;
  const auto admission = f.clock.admit(f.ahead_by(4s), "peer-a");
  EXPECT_EQ(admission.verdict, AdmitVerdict::admitted);
  EXPECT_TRUE(admission.admitted());
  EXPECT_TRUE(admission.receipt.merged);
  EXPECT_TRUE(admission.reason.empty());
  EXPECT_GT(f.clock.now(), f.ahead_by(4s));
}

TEST(HlcAdmit, AStampPastTheBoundIsRefusedSayingHowFarAndWhose) {
  Fixture f;
  const auto admission = f.clock.admit(f.ahead_by(60s), "peer-a");
  EXPECT_EQ(admission.verdict, AdmitVerdict::ahead);
  EXPECT_FALSE(admission.admitted());
  EXPECT_FALSE(admission.receipt.merged);
  EXPECT_NE(admission.reason.find("peer-a"), std::string::npos) << admission.reason;
  EXPECT_NE(admission.reason.find("60000 ms ahead"), std::string::npos) << admission.reason;
  // Not pulled forward: this node's next stamp is not past the refused one.
  EXPECT_LT(f.clock.now(), f.ahead_by(60s));
}

TEST(HlcAdmit, ANodeOutOfBoundsStopsIssuingAtOnce) {
  Fixture f;
  f.clock.set_out_of_bounds(true);
  EXPECT_FALSE(f.clock.issuing());
  EXPECT_THROW((void)f.clock.now(), ClockOutOfBounds);
  try {
    (void)f.clock.now();
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("out of bounds"), std::string::npos);
  }
  f.clock.set_out_of_bounds(false);
  EXPECT_TRUE(f.clock.issuing());
  EXPECT_NO_THROW((void)f.clock.now());
}

TEST(HlcAdmit, ANodeOutOfBoundsRefusesEveryStampItCannotJudge) {
  Fixture f;
  f.clock.set_out_of_bounds(true);
  const auto admission = f.clock.admit(f.ahead_by(1s), "peer-a");
  EXPECT_EQ(admission.verdict, AdmitVerdict::self_out_of_bounds);
  EXPECT_FALSE(admission.admitted());
  EXPECT_NE(admission.reason.find("this node's clock"), std::string::npos) << admission.reason;
}

TEST(HlcAdmit, ReceivingNeverThrowsWhileOutOfBounds) {
  Fixture f;
  const auto before = f.clock.now();
  f.clock.set_out_of_bounds(true);
  EXPECT_NO_THROW((void)f.clock.receive(f.ahead_by(60s)));
  EXPECT_NO_THROW((void)f.clock.receive(f.ahead_by(1s), "peer-a"));
  f.clock.set_out_of_bounds(false);
  EXPECT_GT(f.clock.now(), before);
}

TEST(HlcAdmit, VerdictsHaveNames) {
  EXPECT_STREQ(admit_verdict_name(AdmitVerdict::admitted), "admitted");
  EXPECT_STREQ(admit_verdict_name(AdmitVerdict::ahead), "ahead");
  EXPECT_STREQ(admit_verdict_name(AdmitVerdict::self_out_of_bounds), "self_out_of_bounds");
  EXPECT_STREQ(admit_verdict_name(static_cast<AdmitVerdict>(200)), "unknown");
}

}  // namespace
}  // namespace dannan::contract
