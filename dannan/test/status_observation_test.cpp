//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Observation windows: the inventory's row "observation windows sampled to
// observation/observations.jsonl". Log-linear histograms, a bounded
// registry, one JSON line per window and per event, a rotated log. That the
// lines are Macha's, byte for byte, is the interop suite's.

#include <gtest/gtest.h>

#include <fstream>

#include "dannan/status/observation.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::status {
namespace {

// Every value lies within its bucket, buckets are contiguous, and a bucket
// is at most an eighth as wide as its lower bound.
TEST(Observation, BucketsAreLogLinearAndContiguous) {
  for (uint64_t v : {0ull, 1ull, 7ull, 8ull, 9ull, 15ull, 16ull, 1000ull, 123456789ull, ~0ull}) {
    const auto b = bucket_of(v);
    EXPECT_LE(bucket_lower(b), v);
    EXPECT_LE(v, bucket_upper(b));
  }
  for (size_t b = 0; b + 1 < bucket_count; ++b) EXPECT_EQ(bucket_lower(b + 1), bucket_upper(b) + 1);
  EXPECT_EQ(bucket_upper(bucket_count - 1), ~0ull);
  EXPECT_LE(bucket_upper(bucket_of(1000)) - bucket_lower(bucket_of(1000)), 1000u / 8);
}

TEST(Observation, QuantilesAndWindows) {
  Histogram histogram;
  for (uint64_t v = 1; v <= 100; ++v) histogram.record(v);
  const auto all = histogram.snapshot();
  EXPECT_EQ(all.count, 100u);
  EXPECT_EQ(all.sum, 5050u);
  EXPECT_EQ(all.max, 100u);
  EXPECT_EQ(all.quantile(1.0), 100u);
  const auto p50 = all.quantile(0.5);
  EXPECT_GE(p50, 50u);
  EXPECT_LE(p50, 56u);
  EXPECT_EQ(HistogramSnapshot{}.quantile(0.5), 0u);
  histogram.record(5);
  const auto window = histogram.snapshot().since(all);
  EXPECT_EQ(window.count, 1u);
  EXPECT_EQ(window.sum, 5u);
  EXPECT_EQ(window.max, 5u);
}

// Bounded: past the series bound a name folds into the overflow series;
// past the event bound the oldest event goes, counted.
TEST(Observation, TheRegistryIsBounded) {
  Observations registry(2, 2);
  registry.add("a");
  registry.add("b");
  registry.add("c", 5);
  registry.record("x", 1);
  registry.record("y", 1);
  registry.record("z", 1);
  for (int i = 0; i < 3; ++i) registry.event({uint64_t(i), "e" + std::to_string(i), {}, {}});
  const auto snapshot = registry.snapshot();
  EXPECT_FALSE(snapshot.counters.contains("c"));
  EXPECT_EQ(snapshot.counters.at(std::string(Observations::overflow_series)), 2u);
  EXPECT_EQ(snapshot.counters.at(std::string(Observations::events_dropped_series)), 1u);
  const auto events = registry.drain_events();
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events.front().name, "e1");
  EXPECT_TRUE(registry.drain_events().empty());
}

TEST(Observation, RouteLabelsAreBounded) {
  EXPECT_EQ(route_label("GET", "/api/v1/status/nodes"), "api GET /api/v1/status/nodes");
  EXPECT_EQ(route_label("GET", "/api/v1/files/0a1b2c/meta/x"), "api GET /api/v1/files/:id/meta");
  EXPECT_EQ(route_label("BREW", "/index.html"), "api OTHER web");
  EXPECT_EQ(route_label("POST", "/api/v2/x"), "api POST /api/:other");
}

// One window: what moved since the last, sorted keys, compact JSON.
TEST(Observation, AWindowRendersAsOneJsonLine) {
  ObservationSnapshot window;
  HistogramSnapshot h;
  h.buckets[3] = 2;
  h.count = 2;
  h.sum = 6;
  h.max = 3;
  window.histograms["rpc \"x\""] = h;
  window.histograms["idle"] = {};
  window.counters["moved"] = 4;
  window.counters["still"] = 0;
  EXPECT_EQ(
      render_window(10, 20, "0.1.0", window, {{"queue", 7}}),
      R"({"counters":{"moved":4},"end_ms":20,"gauges":{"queue":7},"histograms":{"rpc \"x\"":{"buckets":[[3,2]],"count":2,"max":3,"p50":3,"p90":3,"p99":3,"sum":6}},"kind":"window","start_ms":10,"version":"0.1.0"})");
  EXPECT_EQ(render_event({5, "start", {{"ms", 9}}, {{"why", "a\\nb"}}}), R"({"at_ms":5,"event":"start","fields":{"ms":9,"why":"a\\nb"},"kind":"event"})");
}

// The log rotates to <path>.1 once a line would take it past its bound.
TEST(Observation, TheLogRotates) {
  test::TempDir dir;
  const auto path = dir.path() / "observation" / "observations.jsonl";
  ObservationLog log(path, 10);
  log.append("12345");
  log.append("67890");
  EXPECT_TRUE(std::filesystem::exists(path.string() + ".1"));
  std::ifstream current(path);
  std::string line;
  std::getline(current, line);
  EXPECT_EQ(line, "67890");
}

// A tick writes the waiting events, then the window since the last tick
// with the gauges sampled now.
TEST(Observation, TheRecorderWritesEventsThenTheWindow) {
  test::TempDir dir;
  const auto path = dir.path() / "observations.jsonl";
  Observations registry;
  test::SteppedWall wall(1000);
  ObservationRecorder recorder(
      registry, std::make_shared<ObservationLog>(path, 1 << 20), "0.1.0", std::chrono::hours(1), [] { return std::map<std::string, uint64_t>{{"g", 1}}; },
      &wall);
  recorder.start();
  registry.add("n", 3);
  registry.event({1500, "something", {}, {}});
  recorder.tick(2000);
  recorder.stop();
  std::ifstream input(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);) lines.push_back(line);
  ASSERT_GE(lines.size(), 2u);
  EXPECT_NE(lines[0].find(R"("kind":"event")"), std::string::npos);
  EXPECT_NE(lines[1].find(R"("start_ms":1000,"version")"), std::string::npos);
  EXPECT_NE(lines[1].find(R"("counters":{"n":3})"), std::string::npos);
  EXPECT_NE(lines[1].find(R"("gauges":{"g":1})"), std::string::npos);
}

// A span timed into a histogram of microseconds, on the injected clock:
// recorded once, at stop() or the end of its scope; cancel() records none.
TEST(ObservedDuration, TimesASpanOnTheInjectedClock) {
  test::SteppedTime time;
  Observations observations;
  {
    ObservedDuration span(observations, "store.barrier_us", &time);
    time.advance(std::chrono::milliseconds(3));
    EXPECT_EQ(span.elapsed_us(), 3000u);
  }
  {
    ObservedDuration span(observations, "store.barrier_us", &time);
    time.advance(std::chrono::microseconds(500));
    span.stop();
    time.advance(std::chrono::seconds(1));
  }
  {
    ObservedDuration abandoned(observations, "store.barrier_us", &time);
    abandoned.cancel();
  }
  const auto snapshot = observations.snapshot();
  const auto& histogram = snapshot.histograms.at("store.barrier_us");
  EXPECT_EQ(histogram.count, 2u);
  EXPECT_EQ(histogram.sum, 3500u);
}

}  // namespace
}  // namespace dannan::status
