//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The thread CPU reporter, against Macha's DIAG lines (src/diagnostics.cpp): quiet
// at DEBUG unless a thread stays hot, with hysteresis and a long repeat.

#include <gtest/gtest.h>

#include <memory>

#include "dannan/status/thread_cpu.h"
#include "dannan/test/fixtures/recording_sink.h"

namespace dannan::status {
namespace {

using namespace std::chrono_literals;

class AtLevel final : public LogSink {
 public:
  explicit AtLevel(LogLevel at) : level(at) {}
  bool enabled(LogLevel at) const noexcept override { return at >= level; }
  void write(LogLevel at, std::string_view, std::string_view message) override { lines.emplace_back(at, std::string(message)); }
  size_t count(std::string_view text) const {
    size_t found = 0;
    for (const auto& [_, line] : lines)
      if (line.find(text) != std::string::npos) ++found;
    return found;
  }
  LogLevel level;
  std::vector<std::pair<LogLevel, std::string>> lines;
};

struct Fixture {
  std::shared_ptr<AtLevel> sink;
  uint64_t cpu = 0;
  ThreadCpuReporter::Clock::time_point now{std::chrono::hours(1)};
  ThreadCpuReporter reporter;
  explicit Fixture(LogLevel level)
      : sink(std::make_shared<AtLevel>(level)), reporter("net-io", 5s, ScopedLogger(sink, "net"), false, [this] { return cpu; }, [this] { return now; }) {}
  // A period at `percent` of a core, with `iterations` ticks' work in it.
  void period(uint64_t percent, uint64_t iterations = 3) {
    cpu += 50'000'000 * percent;
    now += 5s;
    reporter.tick(iterations);
  }
};

TEST(ThreadCpu, AColdThreadSaysNothingAtDebug) {
  Fixture f(LogLevel::debug);
  for (int i = 0; i < 5; ++i) f.period(9);
  EXPECT_TRUE(f.sink->lines.empty());
}

TEST(ThreadCpu, AHotThreadIsNoticedRepeatedAfterAMinuteAndSeenToRecover) {
  Fixture f(LogLevel::debug);
  f.period(50);
  ASSERT_EQ(f.sink->count("DIAG high thread CPU"), 1u);
  EXPECT_EQ(f.sink->lines[0].second, "DIAG high thread CPU name=net-io wall_ms=5000 cpu_ms=2500 cpu_pct=50.0 iterations=3");
  // Quiet within the minute, whether or not it stays hot.
  for (int i = 0; i < 11; ++i) f.period(50);
  EXPECT_EQ(f.sink->count("DIAG high thread CPU"), 1u);
  f.period(50);
  EXPECT_EQ(f.sink->count("DIAG high thread CPU"), 2u);
  // Between the two thresholds it has not recovered.
  f.period(7);
  EXPECT_EQ(f.sink->count("recovered"), 0u);
  f.period(5);
  EXPECT_EQ(f.sink->count("DIAG thread CPU recovered name=net-io"), 1u);
  f.period(1);
  EXPECT_EQ(f.sink->count("recovered"), 1u);
}

TEST(ThreadCpu, EveryPeriodIsTracedAtAll) {
  Fixture f(LogLevel::all);
  f.period(0, 0);
  EXPECT_TRUE(f.sink->lines.empty());
  f.period(0, 1);
  f.period(40);
  EXPECT_EQ(f.sink->count("DIAG thread name=net-io"), 2u);
  EXPECT_EQ(f.sink->count("high"), 0u);
}

TEST(ThreadCpu, NothingIsReportedInsideAPeriod) {
  Fixture f(LogLevel::debug);
  f.cpu += 4'000'000'000;
  f.now += 4s;
  f.reporter.tick();
  EXPECT_TRUE(f.sink->lines.empty());
}

TEST(ThreadCpu, ThisThreadsCpuMoves) {
  const auto before = thread_cpu_ns();
  volatile uint64_t sum = 0;
  for (uint64_t i = 0; i < 20'000'000; ++i) sum = sum + i;
  EXPECT_GT(thread_cpu_ns(), before);
}

}  // namespace
}  // namespace dannan::status
