//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The hybrid logical clock, from its design's tests (TODO/2026-11-15-hlc-design.md, "Tests,
// first"), on a stepped wall clock and a ceiling kept in memory.

#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "dannan/contract/hlc.h"

namespace dannan::contract {
namespace {

using namespace std::chrono_literals;

class SteppedWall final : public WallClock {
 public:
  explicit SteppedWall(uint64_t ms) : _ms(ms) {}
  uint64_t unix_ms() const noexcept override { return _ms.load(); }
  void set(uint64_t ms) { _ms.store(ms); }
  void step(int64_t by) { _ms.store(static_cast<uint64_t>(static_cast<int64_t>(_ms.load()) + by)); }

 private:
  std::atomic<uint64_t> _ms;
};

// A ceiling kept in memory, standing for the node's sealed counters across a restart.
class MemoryCeiling final : public HlcCeiling {
 public:
  uint64_t load() override { return value; }
  void raise(uint64_t unix_ms) override {
    if (failing) throw std::runtime_error("the state device failed");
    value = std::max(value, unix_ms);
    ++raises;
  }
  uint64_t value = 0;
  int raises = 0;
  bool failing = false;
};

constexpr uint64_t t0 = 1'790'000'000'000;

TEST(Hlc, NeverGoesBackWhenTheWallStandsStepsBackOrJumps) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HybridClock clock(wall, ceiling);
  HlcStamp last = clock.now();
  for (int i = 0; i < 100; ++i) {
    if (i == 30) wall.step(-5000);
    if (i == 60) wall.step(+20000);
    if (i % 10 == 0) clock.keep_ahead();
    const auto stamp = clock.now();
    EXPECT_GT(stamp, last) << i;
    last = stamp;
  }
  // After the jump the physical part is the wall's.
  EXPECT_EQ(last.unix_ms(), wall.unix_ms());
}

TEST(Hlc, AStampWithinTheBoundIsMergedAndOneBeyondIsNot) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HybridClock clock(wall, ceiling, 60s);
  const auto near = HlcStamp::of(t0 + 10'000, 7);
  const auto merged = clock.receive(near);
  EXPECT_TRUE(merged.merged);
  EXPECT_GT(merged.local, near);
  EXPECT_EQ(merged.offset, 10'000ms);
  EXPECT_GT(clock.now(), near);
  const auto far = HlcStamp::of(t0 + 120'000, 0);
  const auto refused = clock.receive(far);
  EXPECT_FALSE(refused.merged);
  EXPECT_EQ(refused.offset, 120'000ms);
  // The clock was not pulled forward to it.
  EXPECT_LT(clock.now(), far);
  EXPECT_EQ(clock.stats().not_merged, 1u);
}

TEST(Hlc, ALogicalOverflowMovesThePhysicalPartAndNeverWraps) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HybridClock clock(wall, ceiling);
  HlcStamp last = clock.now();
  for (int i = 0; i < 70'000; ++i) {
    const auto stamp = clock.now();
    ASSERT_GT(stamp, last);
    last = stamp;
  }
  EXPECT_GE(clock.stats().overflows, 1u);
  // 70,001 stamps in one wall millisecond: one overflow.
  EXPECT_EQ(clock.stats().overflows, 1u);
  EXPECT_EQ(last.unix_ms(), t0 + 1);
}

// The wall set back across a restart: the clock begins at the ceiling, after every stamp before.
TEST(Hlc, ARestartAfterTheWallWasSetBackStillIssuesLaterStamps) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HlcStamp before;
  {
    HybridClock clock(wall, ceiling);
    for (int i = 0; i < 1000; ++i) before = clock.now();
  }
  wall.step(-60'000);
  HybridClock again(wall, ceiling);
  EXPECT_GT(again.now(), before);
}

// Dagda's and Lir's test: a receive 30 s ahead, then a crash before the roster loop's next raise.
// The raise the receive needed was made before it returned, so after the restart the first stamp
// is after the received one.
TEST(Hlc, AReceiveFarAheadThenACrashBeforeTheLoopRaisesIsStillAfter) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HlcStamp received;
  {
    // A bound wide enough that the stamp 30 s ahead is merged: the case is the raise, not the bound.
    HybridClock clock(wall, ceiling, std::chrono::minutes(1));
    received = clock.receive(HlcStamp::of(t0 + 30'000, 5)).local;
    EXPECT_EQ(clock.stats().raises_off_loop, 1u);
  }
  HybridClock again(wall, ceiling, std::chrono::minutes(1));
  EXPECT_GT(again.now(), received);
}

// The roster loop keeps the ceiling ahead, so the write path never raises in the steady state.
// From 0.12.0 the first stamp raises once on the write path (the constructor writes nothing);
// after it, none.
TEST(Hlc, TheLoopKeepsTheCeilingAheadSoTheWritePathNeverWaits) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HybridClock clock(wall, ceiling);
  (void)clock.now();
  EXPECT_EQ(clock.stats().raises_off_loop, 1u);
  clock.keep_ahead();
  for (int second = 0; second < 30; ++second) {
    for (int i = 0; i < 100; ++i) {
      wall.step(10);
      (void)clock.now();
    }
    clock.keep_ahead();
    EXPECT_GE(clock.ceiling(), wall.unix_ms() + 2000);
  }
  EXPECT_EQ(clock.stats().raises_off_loop, 1u);
  EXPECT_EQ(ceiling.value, clock.ceiling());
}

// Lir's finding (2026-11-19): every node rewrote its ceiling once a second, idle or not. A clock
// that issues nothing writes nothing: not at start, and not over a minute of the roster loop
// with the wall moving.
TEST(Hlc, AnIdleClockWritesNothing) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HybridClock clock(wall, ceiling);
  for (int second = 0; second < 60; ++second) {
    wall.step(1000);
    clock.keep_ahead();
  }
  EXPECT_EQ(ceiling.raises, 0);
  // The first stamp after the idle minute raises once, before it is issued, past itself.
  const auto stamp = clock.now();
  EXPECT_EQ(ceiling.raises, 1);
  EXPECT_GE(ceiling.value, stamp.unix_ms());
  EXPECT_EQ(clock.stats().raises_off_loop, 1u);
  // Then idle again: nothing more.
  for (int second = 0; second < 60; ++second) {
    wall.step(1000);
    clock.keep_ahead();
  }
  EXPECT_EQ(ceiling.raises, 1);
}

// A restart that issued nothing leaves the ceiling as the run before left it, and still begins
// after every stamp that run issued.
TEST(Hlc, ARestartThatIssuesNothingLeavesTheCeilingAndStaysAfter) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HlcStamp before;
  {
    HybridClock clock(wall, ceiling);
    before = clock.now();
  }
  const auto left = ceiling.value;
  const auto raises = ceiling.raises;
  {
    HybridClock idle(wall, ceiling);
  }
  EXPECT_EQ(ceiling.value, left);
  EXPECT_EQ(ceiling.raises, raises);
  wall.step(-60'000);
  HybridClock again(wall, ceiling);
  EXPECT_GT(again.now(), before);
}

// A ceiling that cannot be raised: the stamp that needed it is not issued.
TEST(Hlc, AStampPastACeilingThatCannotBeRaisedIsNotIssued) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HybridClock clock(wall, ceiling);
  ceiling.failing = true;
  wall.step(10'000);
  EXPECT_THROW((void)clock.now(), std::runtime_error);
  ceiling.failing = false;
  EXPECT_EQ(clock.now().unix_ms(), wall.unix_ms());
}

TEST(Hlc, ManyThreadsGetDistinctStampsEachIncreasing) {
  SteppedWall wall(t0);
  MemoryCeiling ceiling;
  HybridClock clock(wall, ceiling);
  constexpr int threads = 8, each = 20'000;
  std::vector<std::vector<uint64_t>> seen(threads);
  std::vector<std::thread> running;
  for (int t = 0; t < threads; ++t)
    running.emplace_back([&, t] {
      for (int i = 0; i < each; ++i) {
        if (t == 0 && i % 1000 == 0) wall.step(1);
        seen[t].push_back(clock.now().value);
      }
    });
  for (auto& thread : running) thread.join();
  std::set<uint64_t> all;
  for (const auto& mine : seen) {
    for (size_t i = 1; i < mine.size(); ++i) ASSERT_GT(mine[i], mine[i - 1]);
    all.insert(mine.begin(), mine.end());
  }
  EXPECT_EQ(all.size(), static_cast<size_t>(threads) * each);
}

TEST(Hlc, TheEncodingRoundTripsAndOrdersAsTheStamps) {
  const auto a = HlcStamp::of(t0, 1), b = HlcStamp::of(t0, 2), c = HlcStamp::of(t0 + 1, 0);
  for (const auto stamp : {a, b, c}) EXPECT_EQ(decode(std::span<const uint8_t, 8>(encode(stamp))), stamp);
  EXPECT_LT(encode(a), encode(b));
  EXPECT_LT(encode(b), encode(c));
  EXPECT_EQ(encode(HlcStamp{0x0102030405060708})[0], 0x01);
}

// Two nodes, the second's wall five seconds behind: an event there after a message from the first
// is after the first's event.
TEST(Hlc, CausallyOrderedEventsOnTwoNodesStayOrdered) {
  SteppedWall wall_a(t0), wall_b(t0 - 5000);
  MemoryCeiling ceiling_a, ceiling_b;
  HybridClock a(wall_a, ceiling_a), b(wall_b, ceiling_b);
  HlcStamp previous{};
  for (int round = 0; round < 50; ++round) {
    const auto sent = a.now();
    EXPECT_GT(sent, previous);
    const auto at_b = b.receive(sent, "a");
    EXPECT_GT(at_b.local, sent);
    const auto reply = b.now();
    EXPECT_GT(reply, at_b.local);
    previous = a.receive(reply, "b").local;
    EXPECT_GT(previous, reply);
    wall_a.step(3);
    wall_b.step(3);
  }
  const auto skew = b.stats().peers.at("a");
  EXPECT_GE(skew.ahead, 4000ms);
  EXPECT_EQ(a.stats().peers.count("b"), 1u);
}

}  // namespace
}  // namespace dannan::contract
