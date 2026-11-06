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
#include <limits>
#include <thread>
#include <tuple>
#include <vector>

#include "dannan/status/observation.h"
#include "dannan/test/fixtures/recording_sink.h"
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

TEST(Observation, EveryValueLiesInItsBucketAndBucketsTileTheRange) {
  size_t previous = 0;
  for (uint64_t value = 0; value <= (1U << 20); ++value) {
    const auto bucket = bucket_of(value);
    ASSERT_GE(bucket, previous);
    ASSERT_LE(bucket_lower(bucket), value);
    ASSERT_LE(value, bucket_upper(bucket));
    previous = bucket;
  }
  for (unsigned shift = 3; shift < 64; ++shift) {
    const auto power = uint64_t{1} << shift;
    for (const auto value : {power - 1, power, power + 1}) {
      EXPECT_LE(bucket_lower(bucket_of(value)), value);
      EXPECT_LE(value, bucket_upper(bucket_of(value)));
    }
  }
  const auto top = std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(bucket_lower(0), 0u);
  EXPECT_EQ(bucket_of(top), bucket_count - 1);
  for (size_t bucket = 0; bucket < bucket_count; ++bucket) {
    const auto lower = bucket_lower(bucket);
    const auto upper = bucket_upper(bucket);
    EXPECT_EQ(bucket_of(lower), bucket);
    EXPECT_EQ(bucket_of(upper), bucket);
    // Exact below 8, at most an eighth above.
    if (bucket < 8)
      EXPECT_EQ(lower, upper);
    else
      EXPECT_LE(upper - lower + 1, lower / 8);
  }
}

HistogramSnapshot recorded(std::initializer_list<uint64_t> values) {
  Histogram histogram;
  for (const auto value : values) histogram.record(value);
  return histogram.snapshot();
}

TEST(Observation, QuantilesClampAndAnInconsistentSnapshotAnswersMax) {
  EXPECT_EQ(HistogramSnapshot{}.quantile(0.5), 0u);
  const auto small = recorded({1, 2, 3, 4, 5, 6, 7});
  EXPECT_EQ(small.quantile(0.0), 1u);
  EXPECT_EQ(small.quantile(-1.0), 1u);
  EXPECT_EQ(small.quantile(0.5), 4u);
  EXPECT_EQ(small.quantile(1.0), 7u);
  EXPECT_EQ(small.quantile(2.0), 7u);
  const auto wide = recorded({100, 1000, 1001});
  EXPECT_EQ(wide.quantile(0.3), bucket_upper(bucket_of(100)));
  EXPECT_EQ(wide.quantile(1.0), 1001u);
  HistogramSnapshot inconsistent;
  inconsistent.count = 3;
  inconsistent.max = 42;
  inconsistent.buckets[1] = 1;
  EXPECT_EQ(inconsistent.quantile(1.0), 42u);
}

TEST(Observation, AWindowIsTheDifferenceAndReversedSaturates) {
  Histogram histogram;
  histogram.record(5);
  histogram.record(900);
  const auto first = histogram.snapshot();
  histogram.record(6);
  histogram.record(7);
  const auto second = histogram.snapshot();
  const auto window = second.since(first);
  EXPECT_EQ(window.count, 2u);
  EXPECT_EQ(window.sum, 13u);
  EXPECT_EQ(window.buckets[6], 1u);
  EXPECT_EQ(window.max, 7u);
  const auto empty = second.since(second);
  EXPECT_EQ(empty.count, 0u);
  EXPECT_EQ(empty.max, 0u);
  const auto reversed = first.since(second);
  EXPECT_EQ(reversed.count, 0u);
  EXPECT_EQ(reversed.sum, 0u);
}

TEST(Observation, AHistogramRecordsFromManyThreads) {
  Histogram histogram;
  std::vector<std::thread> threads;
  for (uint64_t thread = 0; thread < 4; ++thread)
    threads.emplace_back([&histogram, thread] {
      for (uint64_t value = 1; value <= 20'000; ++value) histogram.record(value * 4 + thread);
    });
  for (auto& thread : threads) thread.join();
  const auto snapshot = histogram.snapshot();
  EXPECT_EQ(snapshot.count, 80'000u);
  EXPECT_EQ(snapshot.max, 80'003u);
  uint64_t expected = 0;
  for (uint64_t thread = 0; thread < 4; ++thread)
    for (uint64_t value = 1; value <= 20'000; ++value) expected += value * 4 + thread;
  EXPECT_EQ(snapshot.sum, expected);
}

TEST(Observation, TheRegistryFoldsPastItsBound) {
  Observations registry(2, 2);
  auto& first = registry.histogram("a");
  EXPECT_EQ(&registry.histogram("a"), &first);
  registry.record("b", 3);
  registry.record("c", 4);
  registry.add("x");
  registry.add("y", 2);
  registry.add("z", 5);
  const auto snapshot = registry.snapshot();
  EXPECT_TRUE(snapshot.histograms.contains("a"));
  EXPECT_FALSE(snapshot.histograms.contains("c"));
  EXPECT_EQ(snapshot.histograms.at(std::string(Observations::overflow_series)).count, 1u);
  EXPECT_EQ(snapshot.counters.at("y"), 2u);
  EXPECT_FALSE(snapshot.counters.contains("z"));
  EXPECT_EQ(snapshot.counters.at(std::string(Observations::overflow_series)), 2u);
  EXPECT_EQ(snapshot.counters.at(std::string(Observations::events_dropped_series)), 0u);
}

TEST(Observation, ASnapshotWindowCountsNewSeriesFromZero) {
  Observations registry;
  registry.add("kept", 3);
  registry.record("latency", 10);
  const auto first = registry.snapshot();
  registry.add("kept", 2);
  registry.add("new", 7);
  registry.record("latency", 11);
  const auto window = registry.snapshot().since(first);
  EXPECT_EQ(window.counters.at("kept"), 2u);
  EXPECT_EQ(window.counters.at("new"), 7u);
  EXPECT_EQ(window.histograms.at("latency").count, 1u);
  EXPECT_EQ(registry.snapshot().since(ObservationSnapshot{}).histograms.at("latency").count, 2u);
}

// Macha's table, row for row.
TEST(Observation, RouteLabelsAreMachas) {
  const std::vector<std::tuple<std::string, std::string, std::string>> cases{
      {"GET", "/api/v1/status", "api GET /api/v1/status"},
      {"POST", "/api/v1/torrents/jobs", "api POST /api/v1/torrents/jobs"},
      {"PUT", "/api/v1/catalogue/items/3f9a0c", "api PUT /api/v1/catalogue/items/:id"},
      {"PATCH", "/api/v1/playback/sessions/abc/segments/12.ts", "api PATCH /api/v1/playback/sessions/abc"},
      {"DELETE", "/api/v1/a//b", "api DELETE /api/v1/a/:id/b"},
      {"GET", "/api/v1/" + std::string(33, 'a'), "api GET /api/v1/:id"},
      {"GET", "/api/v1/Status", "api GET /api/v1/:id"},
      {"GET", "/api/v1/~status", "api GET /api/v1/:id"},
      {"GET", "/api/v1/media_information/x-y", "api GET /api/v1/media_information/x-y"},
      {"GET", "/api/v1/", "api GET /api/v1"},
      {"GET", "/api/v2/status", "api GET /api/:other"},
      {"HEAD", "/index.html", "api OTHER web"},
      {"GET", "/", "api GET web"},
  };
  for (const auto& [method, path, label] : cases) EXPECT_EQ(route_label(method, path), label) << method << " " << path;
}

std::vector<std::string> lines_of(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<std::string> out;
  for (std::string line; std::getline(in, line);) out.push_back(line);
  return out;
}

TEST(Observation, RotationEdges) {
  test::TempDir dir;
  const auto path = dir.path() / "nested" / "observations.jsonl";
  auto rotated = path;
  rotated += ".1";
  ObservationLog log(path, 10);
  log.append("12345");
  log.append("abc");
  log.append("more");
  EXPECT_EQ(lines_of(path), std::vector<std::string>{"more"});
  EXPECT_EQ(lines_of(rotated), (std::vector<std::string>{"12345", "abc"}));
  log.append("0123456789");
  EXPECT_EQ(lines_of(path), std::vector<std::string>{"0123456789"});
  // An existing empty file is appended to, never rotated.
  const auto empty = dir.path() / "empty.jsonl";
  {
    std::ofstream touch(empty);
  }
  ObservationLog empty_log(empty, 1);
  empty_log.append("first");
  EXPECT_EQ(lines_of(empty), std::vector<std::string>{"first"});
  auto empty_rotated = empty;
  empty_rotated += ".1";
  EXPECT_FALSE(std::filesystem::exists(empty_rotated));
  // The newline counts.
  const auto edge = dir.path() / "edge.jsonl";
  ObservationLog edge_log(edge, 10);
  edge_log.append("12345");
  edge_log.append("abcd");
  EXPECT_EQ(lines_of(edge), std::vector<std::string>{"abcd"});
}

TEST(Observation, AnUnwritablePathIsSaidOncePerRunOfFailures) {
  test::TempDir dir;
  const auto path = dir.path() / "observations.jsonl";
  auto journal = std::make_shared<test::RecordingSink>();
  std::filesystem::create_directories(path);
  ObservationLog log(path, 1024, journal);
  log.append("lost");
  log.append("lost again");
  EXPECT_EQ(journal->count(LogLevel::debug, "observation: cannot write"), 1u);
  std::filesystem::remove(path);
  log.append("kept");
  EXPECT_EQ(lines_of(path), std::vector<std::string>{"kept"});
  std::filesystem::remove(path);
  std::filesystem::create_directories(path);
  log.append("lost after a success");
  EXPECT_EQ(journal->count(LogLevel::debug, "observation: cannot write"), 2u);
}

// A sampler's std::exception costs the gauges, not the window, and is said; anything
// else leaves tick() for the supervised loop to record.
TEST(Observation, ASamplerFailureLosesOnlyTheGauges) {
  test::TempDir dir;
  const auto path = dir.path() / "observations.jsonl";
  auto journal = std::make_shared<test::RecordingSink>();
  Observations registry;
  auto log = std::make_shared<ObservationLog>(path, 1 << 20, journal);
  int samples = 0;
  ObservationRecorder recorder(registry, log, "v", std::chrono::hours(1), [&] {
    if (++samples == 2) throw std::runtime_error("sampler failed");
    return std::map<std::string, uint64_t>{{"sample", static_cast<uint64_t>(samples)}};
  });
  recorder.tick(100);
  recorder.tick(200);
  const auto lines = lines_of(path);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_NE(lines[0].find("\"sample\":1"), std::string::npos) << lines[0];
  EXPECT_EQ(lines[1].find("sample"), std::string::npos) << lines[1];
  EXPECT_EQ(journal->count(LogLevel::debug, "observation: gauge sampling failed: sampler failed"), 1u);
  ObservationRecorder strange(registry, log, "v", std::chrono::hours(1), []() -> std::map<std::string, uint64_t> { throw 42; });
  EXPECT_ANY_THROW(strange.tick(300));
  EXPECT_EQ(lines_of(path).size(), 2u);
}

TEST(Observation, TheRecorderWritesAFinalWindowAndASecondStopNothing) {
  test::TempDir dir;
  const auto path = dir.path() / "observations.jsonl";
  Observations registry;
  auto log = std::make_shared<ObservationLog>(path, 1 << 20);
  {
    ObservationRecorder never(registry, log, "v", std::chrono::milliseconds(1), {});
    never.stop();
  }
  EXPECT_FALSE(std::filesystem::exists(path));
  ObservationRecorder recorder(registry, log, "v", std::chrono::milliseconds(5), {});
  recorder.start();
  for (int i = 0; i < 2000 && lines_of(path).size() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const auto periodic = lines_of(path).size();
  EXPECT_GE(periodic, 2u);
  registry.event({1, "shutdown", {}, {}});
  recorder.stop();
  const auto lines = lines_of(path);
  ASSERT_GE(lines.size(), periodic + 2);
  EXPECT_NE(lines[lines.size() - 2].find("shutdown"), std::string::npos);
  recorder.stop();
  EXPECT_EQ(lines_of(path).size(), lines.size());
}

}  // namespace
}  // namespace dannan::status
