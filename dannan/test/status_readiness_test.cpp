//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Readiness, against Macha's RecoveryProgress (src/cluster/local_state.hpp):
// planes marked one by one, the first failure kept, completion that may wait for
// an explicit mark, and waiters woken by either.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <thread>
#include <vector>

#include "dannan/status/readiness.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::status {
namespace {

using namespace std::chrono_literals;

TEST(Readiness, IsCompleteWhenEveryPlaneIsReady) {
  test::SteppedWall wall;
  Readiness readiness({"data", "metadata"}, &wall);
  EXPECT_FALSE(readiness.complete());
  readiness.ready("data");
  EXPECT_TRUE(readiness.has("data"));
  EXPECT_FALSE(readiness.has("metadata"));
  EXPECT_FALSE(readiness.complete());
  EXPECT_EQ(readiness.ready_unix_ms(), 0u);
  readiness.ready("metadata");
  EXPECT_TRUE(readiness.complete());
  EXPECT_NE(readiness.ready_unix_ms(), 0u);
  EXPECT_THROW(readiness.ready("nothing"), std::invalid_argument);
}

// Macha marks complete only after recovery returns, later than its planes, so a
// node is not ready, and its zero capacity not trusted, while recovery runs.
TEST(Readiness, HeldItWaitsForTheMarkAfterItsPlanes) {
  test::SteppedWall wall;
  Readiness readiness({"data"}, &wall);
  readiness.hold_until_marked();
  readiness.control_plane(true);
  readiness.ready("data");
  EXPECT_FALSE(readiness.complete());
  EXPECT_EQ(readiness.phase(), Phase::recovering);
  EXPECT_EQ(readiness.ready_unix_ms(), 0u);
  wall.advance(5ms);
  const auto marked_at = wall.unix_ms();
  readiness.mark_complete();
  EXPECT_TRUE(readiness.complete());
  EXPECT_EQ(readiness.phase(), Phase::ready);
  EXPECT_EQ(readiness.ready_unix_ms(), marked_at);
}

TEST(Readiness, AMarkBeforeItsPlanesDoesNotComplete) {
  test::SteppedWall wall;
  Readiness readiness({"data"}, &wall);
  readiness.hold_until_marked();
  readiness.mark_complete();
  EXPECT_FALSE(readiness.complete());
  readiness.ready("data");
  EXPECT_TRUE(readiness.complete());
}

TEST(Readiness, HoldingAfterTheNodeStartsIsRefused) {
  test::SteppedWall wall;
  Readiness readiness({}, &wall);
  readiness.close_declarations();
  EXPECT_THROW(readiness.hold_until_marked(), std::logic_error);
}

TEST(Readiness, KeepsTheFirstFailureAndWakesItsWaiters) {
  test::SteppedWall wall;
  Readiness readiness({"data"}, &wall);
  std::thread failer([&] {
    readiness.fail("the disk is gone");
    readiness.fail("and so a later thing broke");
  });
  EXPECT_FALSE(readiness.wait_complete(10s));
  failer.join();
  EXPECT_TRUE(readiness.failed());
  EXPECT_EQ(readiness.error(), "the disk is gone");
}

TEST(Readiness, AMarkWakesAWaiter) {
  test::SteppedWall wall;
  Readiness readiness({}, &wall);
  readiness.hold_until_marked();
  std::thread marker([&] { readiness.mark_complete(); });
  EXPECT_TRUE(readiness.wait_complete(10s));
  marker.join();
}

// Health (item 7, TODO/2026-11-23-readiness-health.md, agreed with Dagda, Louhi, Kvasir and Lugh):
// a node's state after startup, the worst of its drain and every check, never figures.

// A started readiness: its one plane ready and its control plane up.
struct Started {
  test::SteppedWall wall;
  Readiness readiness{{"data"}, &wall};
  Started() {
    readiness.control_plane(true);
    readiness.ready("data");
  }
};

TEST(Health, NotReadyUntilStartupIsCompleteAndAfterAFailure) {
  test::SteppedWall wall;
  Readiness readiness({"data"}, &wall);
  EXPECT_EQ(readiness.health(), Health::not_ready);
  readiness.control_plane(true);
  EXPECT_EQ(readiness.health(), Health::not_ready);
  readiness.ready("data");
  EXPECT_EQ(readiness.health(), Health::ready);
  readiness.fail("the disk is gone");
  EXPECT_EQ(readiness.health(), Health::not_ready);
}

TEST(Health, TheWorstCheckWins) {
  Started node;
  auto& readiness = node.readiness;
  readiness.set("a.cpu", Health::degraded, "cpu limits missing");
  EXPECT_EQ(readiness.health(), Health::degraded);
  readiness.set("a.docker", Health::not_ready, "docker did not answer");
  EXPECT_EQ(readiness.health(), Health::not_ready);
  readiness.set("a.docker", Health::ready);
  EXPECT_EQ(readiness.health(), Health::degraded);
  readiness.clear("a.cpu");
  EXPECT_EQ(readiness.health(), Health::ready);
  readiness.clear("never.set");
  EXPECT_THROW(readiness.set("", Health::ready), std::invalid_argument);
}

TEST(Health, ADrainIsDrainingUnlessSomethingIsWorse) {
  Started node;
  auto& readiness = node.readiness;
  readiness.set("a.cpu", Health::degraded);
  readiness.drain(true, "retiring");
  EXPECT_EQ(readiness.health(), Health::draining);
  readiness.set("a.docker", Health::not_ready);
  EXPECT_EQ(readiness.health(), Health::not_ready);
  readiness.clear("a.docker");
  readiness.drain(false);
  EXPECT_EQ(readiness.health(), Health::degraded);
}

// Louhi's loop: a verdict not set again within valid_for reads as stopped reporting.
TEST(Health, AVerdictNotRenewedInTimeReadsNotReady) {
  Started node;
  auto& readiness = node.readiness;
  readiness.set("a.docker", Health::ready, {}, 30s);
  node.wall.advance(29s);
  EXPECT_EQ(readiness.health(), Health::ready);
  node.wall.advance(1s);
  EXPECT_EQ(readiness.health(), Health::not_ready);
  const auto checks = readiness.checks();
  ASSERT_EQ(checks.size(), 1u);
  EXPECT_EQ(checks[0].health, Health::not_ready);
  EXPECT_NE(checks[0].reason.find("stopped reporting"), std::string::npos) << checks[0].reason;
  readiness.set("a.docker", Health::ready, {}, 30s);
  EXPECT_EQ(readiness.health(), Health::ready);
}

// Louhi's ask: ordered by name, each with its reason and when it last changed.
TEST(Health, ChecksAreOrderedByNameWithTheirReasonAndSince) {
  Started node;
  auto& readiness = node.readiness;
  readiness.set("z.last", Health::degraded, "slow");
  node.wall.advance(5s);
  readiness.set("a.first", Health::ready);
  readiness.drain(true, "retiring");
  const auto first_set = node.wall.unix_ms() - 5000;
  node.wall.advance(5s);
  // The same verdict again: its since stays.
  readiness.set("z.last", Health::degraded, "still slow");
  const auto checks = readiness.checks();
  ASSERT_EQ(checks.size(), 3u);
  EXPECT_EQ(checks[0].name, "a.first");
  EXPECT_EQ(checks[1].name, "dannan.drain");
  EXPECT_EQ(checks[1].health, Health::draining);
  EXPECT_EQ(checks[1].reason, "retiring");
  EXPECT_EQ(checks[2].name, "z.last");
  EXPECT_EQ(checks[2].reason, "still slow");
  EXPECT_EQ(checks[2].since_unix_ms, first_set);
}

TEST(Health, AChangeIsPostedNeverCalledInlineAndOncePerChange) {
  Started node;
  auto& readiness = node.readiness;
  boost::asio::io_context io;
  std::vector<Health> seen;
  const auto id = readiness.on_change(io.get_executor(), [&](Health health) { seen.push_back(health); });
  readiness.set("a.cpu", Health::degraded);
  readiness.set("a.cpu", Health::degraded, "again");
  readiness.set("a.other", Health::ready);
  EXPECT_TRUE(seen.empty());
  io.run();
  EXPECT_EQ(seen, std::vector<Health>{Health::degraded});
  readiness.cancel(id);
  readiness.clear("a.cpu");
  io.restart();
  io.run();
  EXPECT_EQ(seen.size(), 1u);
}

// An expiry is a change too, found when the node's status step evaluates.
TEST(Health, AnExpiryIsToldWhenEvaluated) {
  Started node;
  auto& readiness = node.readiness;
  boost::asio::io_context io;
  std::vector<Health> seen;
  (void)readiness.on_change(io.get_executor(), [&](Health health) { seen.push_back(health); });
  readiness.set("a.docker", Health::ready, {}, 10s);
  node.wall.advance(11s);
  readiness.evaluate();
  readiness.evaluate();
  io.run();
  EXPECT_EQ(seen, std::vector<Health>{Health::not_ready});
}

TEST(Health, ItsNamesAndAPeersAdvert) {
  EXPECT_STREQ(health_name(Health::ready), "ready");
  EXPECT_STREQ(health_name(Health::degraded), "degraded");
  EXPECT_STREQ(health_name(Health::draining), "draining");
  EXPECT_STREQ(health_name(Health::not_ready), "not_ready");
  membership::NodeRecord record;
  EXPECT_EQ(health_of(record), Health::ready) << "a node before 0.12.0 says nothing";
  set_health_advert(record, Health::draining);
  EXPECT_EQ(health_of(record), Health::draining);
  set_health_advert(record, Health::ready);
  EXPECT_EQ(health_of(record), Health::ready);
}

}  // namespace
}  // namespace dannan::status
