//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Keyed limits (item 10; Lugh's, Mimir's, Kvasir's and Macha's asks): token buckets keyed by a
// caller's string, a refusal saying how long to wait for Retry-After, and a hard bound on the
// keys: full buckets dropped first, as they hold nothing a new key would not, then the least
// recently touched.

#include <gtest/gtest.h>

#include <string>

#include "dannan/listen/rate_limiter.h"

namespace dannan::listen {
namespace {

using namespace std::chrono_literals;
using Policy = RateLimiter::Policy;

const contract::TimePoint start{std::chrono::hours(1)};

TEST(RateLimiter, ABurstThenAWaitRoundedUpToASecond) {
  RateLimiter limits;
  const Policy policy{3, 60};
  for (int i = 0; i < 3; ++i) EXPECT_FALSE(limits.take("a", policy, start)) << i;
  EXPECT_EQ(limits.take("a", policy, start), 1s);
  // A token a second: half a second in, still a second to say.
  EXPECT_EQ(limits.take("a", policy, start + 500ms), 1s);
  EXPECT_FALSE(limits.take("a", policy, start + 1s));
  EXPECT_EQ(limits.take("a", policy, start + 1s), 1s);
}

TEST(RateLimiter, TheWaitIsTheTimeToTheNextToken) {
  RateLimiter limits;
  const Policy policy{1, 2};
  EXPECT_FALSE(limits.take("a", policy, start));
  EXPECT_EQ(limits.take("a", policy, start), 30s);
  EXPECT_EQ(limits.take("a", policy, start + 10s), 20s);
  EXPECT_FALSE(limits.take("a", policy, start + 30s));
}

TEST(RateLimiter, ABucketRefillsNoFurtherThanItsBurst) {
  RateLimiter limits;
  const Policy policy{2, 60};
  EXPECT_FALSE(limits.take("a", policy, start));
  for (int i = 0; i < 2; ++i) EXPECT_FALSE(limits.take("a", policy, start + 1h)) << i;
  EXPECT_TRUE(limits.take("a", policy, start + 1h));
}

TEST(RateLimiter, ZeroIsUnlimitedAndKeepsNoKey) {
  RateLimiter limits;
  for (int i = 0; i < 1000; ++i) {
    EXPECT_FALSE(limits.take("a", Policy{0, 60}, start));
    EXPECT_FALSE(limits.take("b", Policy{60, 0}, start));
  }
  EXPECT_EQ(limits.size(), 0u);
}

// Mimir's proof: ten failed AUTHs from one address are delayed, another address is not.
TEST(RateLimiter, KeysAreSeparate) {
  RateLimiter limits;
  const Policy policy{5, 6};
  for (int i = 0; i < 10; ++i) (void)limits.take("10.0.0.1", policy, start);
  EXPECT_TRUE(limits.take("10.0.0.1", policy, start));
  EXPECT_FALSE(limits.take("10.0.0.2", policy, start));
}

TEST(RateLimiter, TheBoundIsHard) {
  RateLimiter limits(64);
  EXPECT_EQ(limits.max_keys(), 64u);
  const Policy policy{5, 1};
  for (int i = 0; i < 10000; ++i) {
    (void)limits.take("k" + std::to_string(i), policy, start);
    ASSERT_LE(limits.size(), 64u) << i;
  }
}

// Lugh's rule: full buckets go first, so a flood of keys that come back to full never evicts a
// key that is still limited.
TEST(RateLimiter, FullBucketsGoFirst) {
  RateLimiter limits(16);
  const Policy slow{5, 1};
  for (int i = 0; i < 5; ++i) (void)limits.take("abuser", slow, start);
  ASSERT_TRUE(limits.take("abuser", slow, start));
  const Policy fast{5, 600};
  auto now = start;
  for (int round = 0; round < 20; ++round) {
    for (int i = 0; i < 15; ++i) (void)limits.take("r" + std::to_string(round) + "-" + std::to_string(i), fast, now);
    // A second on, every flood bucket is full again; the abuser's, at one a minute, is not.
    now += 1s;
  }
  EXPECT_TRUE(limits.take("abuser", slow, now));
}

// With none full, the key touched longest ago goes.
TEST(RateLimiter, ThenTheLeastRecentlyTouched) {
  RateLimiter limits(8);
  const Policy policy{1, 1};
  (void)limits.take("oldest", policy, start);
  for (int i = 0; i < 7; ++i) (void)limits.take("k" + std::to_string(i), policy, start + 1s);
  // Touched again, so no longer the oldest.
  (void)limits.take("k0", policy, start + 2s);
  (void)limits.take("newest", policy, start + 3s);
  EXPECT_LE(limits.size(), 8u);
  // Evicted: a new bucket, so its first take is allowed.
  EXPECT_FALSE(limits.take("oldest", policy, start + 3s));
  EXPECT_TRUE(limits.take("k0", policy, start + 3s));
  EXPECT_TRUE(limits.take("newest", policy, start + 3s));
}

TEST(RateLimiter, TimeGoingBackRefillsNothing) {
  RateLimiter limits;
  const Policy policy{1, 60};
  EXPECT_FALSE(limits.take("a", policy, start));
  EXPECT_TRUE(limits.take("a", policy, start - 10s));
}

}  // namespace
}  // namespace dannan::listen
