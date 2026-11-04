//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Activity per work class, node events, and the Status surface: the
// inventory's "Telemetry and status" rows for activity clocks and node
// events, and the design's Status noun (publish, counter).

#include <gtest/gtest.h>

#include <thread>

#include "dannan/status/activity.h"
#include "dannan/status/events.h"
#include "dannan/status/surface.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::status {
namespace {

using namespace std::chrono_literals;
using sched::FrameType;
using sched::WorkClass;

// Presence is per work class, with or without bytes; bytes stay per frame.
TEST(ActivityClocks, PresenceIsPerClassAndBytesPerFrame) {
  test::SteppedTime time;
  ActivityClocks clocks(&time);
  EXPECT_EQ(clocks.idle_for(WorkClass::foreground), 24h);
  clocks.note(FrameType::read_ahead, 100);
  clocks.note(FrameType::foreground, 50);
  time.advance(3s);
  EXPECT_EQ(clocks.idle_for(WorkClass::foreground), 3s);
  EXPECT_TRUE(clocks.recently_active(WorkClass::foreground, 4s));
  EXPECT_FALSE(clocks.recently_active(WorkClass::foreground, 3s));
  EXPECT_EQ(clocks.idle_for(WorkClass::loader), 24h);
  clocks.note(WorkClass::loader);  // an import's request, no bytes
  EXPECT_EQ(clocks.idle_for(WorkClass::loader), 0ms);
  EXPECT_EQ(clocks.take_bytes(FrameType::read_ahead), 100u);
  EXPECT_EQ(clocks.take_bytes(FrameType::read_ahead), 0u);
  EXPECT_EQ(clocks.take_bytes(FrameType::foreground), 50u);
}

// Producers only count; a consumer diffs the counts and waits for any to
// move, and never misses one.
TEST(NodeEvents, CountsPerKindAndWakesAWaiter) {
  NodeEvents events;
  const auto stored = events.kind("storage");
  EXPECT_EQ(events.kind("storage"), stored);
  EXPECT_NE(stored, NodeEvents::topology);
  EXPECT_EQ(events.count(stored), 0u);
  const auto before = events.total();
  std::thread producer([&] { events.notify(stored); });
  EXPECT_TRUE(events.wait_for_change(before, 5s));
  producer.join();
  EXPECT_EQ(events.count(stored), 1u);
  EXPECT_EQ(events.count(NodeEvents::topology), 0u);
  EXPECT_FALSE(events.wait_for_change(events.total(), 10ms));
}

TEST(NodeEvents, KindsAreBounded) {
  NodeEvents events;
  for (size_t i = NodeEvents::dannan_kinds; i < NodeEvents::max_kinds; ++i) events.kind("kind " + std::to_string(i));
  EXPECT_THROW(events.kind("one too many"), std::length_error);
}

// What a hosted system publishes, and its counters, as one snapshot.
TEST(StatusSurface, PublishesValuesAndCounters) {
  Surface surface;
  surface.publish("fs.mount", std::string("/srv/media"));
  surface.publish("fs.parked", uint64_t{2});
  surface.publish("fs.parked", uint64_t{3});
  auto& requests = surface.counter("fs.requests");
  requests.fetch_add(5);
  surface.counter("fs.requests").fetch_add(1);
  const auto snapshot = surface.snapshot();
  EXPECT_EQ(std::get<std::string>(snapshot.values.at("fs.mount")), "/srv/media");
  EXPECT_EQ(std::get<uint64_t>(snapshot.values.at("fs.parked")), 3u);
  EXPECT_EQ(snapshot.counters.at("fs.requests"), 6u);
  surface.withdraw("fs.mount");
  EXPECT_FALSE(surface.snapshot().values.contains("fs.mount"));
}

}  // namespace
}  // namespace dannan::status
