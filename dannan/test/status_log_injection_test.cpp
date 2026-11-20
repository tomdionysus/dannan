//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Logging for injection (TODO/2026-11-20-stack-plan.md, item 28; the maintainer, 2026-11-20: "Macha would
// thread a logger and the Dannan instance"). A root owns one sink from before any node exists,
// may swap what is behind it while other threads log, and hands it on; an emit may bypass the
// filter from a thread no component owns (FFmpeg's, libtorrent's).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dannan/status/log.h"
#include "dannan/test/fixtures/recording_sink.h"

namespace dannan::status {
namespace {

// Records from `floor` up.
class Floor final : public LogSink {
 public:
  explicit Floor(LogLevel floor) : _floor(floor) {}
  bool enabled(LogLevel level) const noexcept override { return static_cast<int>(level) >= static_cast<int>(_floor); }
  void write(LogLevel, std::string_view, std::string_view) override { ++written; }
  std::atomic<int> written{0};

 private:
  const LogLevel _floor;
};

TEST(SwappableSink, AnswersFromItsFirstSinkBeforeAnyIsSet) {
  auto first = std::make_shared<test::RecordingSink>();
  SwappableSink sink(first);
  EXPECT_TRUE(sink.enabled(LogLevel::debug));
  sink.write(LogLevel::info, "root", "loading config");
  EXPECT_EQ(first->count(LogLevel::info, "loading config"), 1u);
  EXPECT_EQ(sink.current(), first);
}

TEST(SwappableSink, ASetSinkTakesTheRecordsAndTheFilter) {
  auto first = std::make_shared<test::RecordingSink>();
  auto second = std::make_shared<Floor>(LogLevel::warn);
  SwappableSink sink(first);
  sink.set(second);
  EXPECT_FALSE(sink.enabled(LogLevel::info));
  EXPECT_TRUE(sink.enabled(LogLevel::warn));
  EXPECT_TRUE(sink.enabled(LogLevel::error));
  sink.write(LogLevel::warn, "root", "after");
  EXPECT_EQ(second->written.load(), 1);
  EXPECT_EQ(first->count(LogLevel::warn, "after"), 0u);
}

TEST(SwappableSink, ALoggerHandedOutBeforeASwapWritesToTheNewSink) {
  auto first = std::make_shared<test::RecordingSink>();
  auto sink = std::make_shared<SwappableSink>(first);
  ScopedLogger logger(sink, "fuse");
  auto second = std::make_shared<test::RecordingSink>();
  sink->set(second);
  logger.info("mounted");
  EXPECT_EQ(second->count(LogLevel::info, "mounted"), 1u);
  EXPECT_EQ(first->count(LogLevel::info, "mounted"), 0u);
}

TEST(SwappableSink, SwapsWhileOtherThreadsLogFreeNoSinkInUse) {
  constexpr int threads = 4;
  constexpr int each = 2000;
  auto sink = std::make_shared<SwappableSink>(std::make_shared<Floor>(LogLevel::all));
  std::atomic<bool> go{false};
  std::atomic<int> done{0};
  std::vector<std::shared_ptr<Floor>> seen;
  seen.push_back(std::static_pointer_cast<Floor>(sink->current()));
  std::vector<std::thread> writers;
  for (int t = 0; t < threads; ++t)
    writers.emplace_back([&] {
      while (!go.load()) {
      }
      for (int i = 0; i < each; ++i)
        if (sink->enabled(LogLevel::info)) sink->write(LogLevel::info, "w", "line");
      ++done;
    });
  go.store(true);
  // Each replaced sink is released here as soon as it is swapped out: only the swap's own
  // handoff may keep it alive for a write still inside it.
  while (done.load() < threads) {
    auto next = std::make_shared<Floor>(LogLevel::all);
    seen.push_back(next);
    sink->set(next);
    if (seen.size() > 8) seen.erase(seen.begin() + 1, seen.end() - 4);
  }
  for (auto& w : writers) w.join();
  // Not every sink is still held here, so the total is not summed; the test is that no write
  // landed in a freed sink (ASan and TSan at the minor's end) and the writers ran through.
  int counted = 0;
  for (const auto& s : seen) counted += s->written.load();
  EXPECT_GT(counted, 0);
}

TEST(SwappableSink, EnabledReadsTheFilterTakenAtSet) {
  auto quiet = std::make_shared<Floor>(LogLevel::error);
  SwappableSink sink(quiet);
  for (const auto level : {LogLevel::all, LogLevel::debug, LogLevel::info, LogLevel::warn}) EXPECT_FALSE(sink.enabled(level));
  EXPECT_TRUE(sink.enabled(LogLevel::error));
}

TEST(SwappableSink, RefusesNoSink) {
  EXPECT_THROW(SwappableSink(nullptr), std::invalid_argument);
  SwappableSink sink(std::make_shared<test::RecordingSink>());
  EXPECT_THROW(sink.set(nullptr), std::invalid_argument);
}

TEST(SwappableSink, WriteAtKeepsTheRecordsTime) {
  class Stamped final : public LogSink {
   public:
    bool enabled(LogLevel) const noexcept override { return true; }
    void write(LogLevel, std::string_view, std::string_view) override {}
    void write_at(std::chrono::system_clock::time_point at, LogLevel, std::string_view, std::string_view) override { seen = at; }
    std::chrono::system_clock::time_point seen{};
  };
  auto stamped = std::make_shared<Stamped>();
  SwappableSink sink(stamped);
  const auto at = std::chrono::system_clock::time_point(std::chrono::seconds(1'700'000'000));
  sink.write_at(at, LogLevel::info, "log", "queued");
  EXPECT_EQ(stamped->seen, at);
}

TEST(ScopedLogger, EmitBypassesTheFilter) {
  auto quiet = std::make_shared<Floor>(LogLevel::error);
  ScopedLogger logger(quiet, "ffmpeg");
  logger.debug("dropped by the filter");
  EXPECT_EQ(quiet->written.load(), 0);
  logger.emit(LogLevel::debug, "kept: the caller filtered already");
  EXPECT_EQ(quiet->written.load(), 1);
}

TEST(ScopedLogger, EmitIsSafeFromAThreadNoComponentOwns) {
  auto recording = std::make_shared<test::RecordingSink>();
  auto sink = std::make_shared<SwappableSink>(recording);
  ScopedLogger logger(sink, "ffmpeg");
  std::thread codec([logger] {
    for (int i = 0; i < 100; ++i) logger.emit(LogLevel::info, "frame");
  });
  codec.join();
  EXPECT_EQ(recording->count(LogLevel::info, "frame"), 100u);
}

TEST(RecordingSink, KeepsScopeLevelAndMessage) {
  test::RecordingSink sink;
  sink.write(LogLevel::warn, "net", "slow peer");
  sink.write(LogLevel::info, "net.client", "dialled");
  EXPECT_EQ(sink.lines(), (std::vector<std::string>{"slow peer", "dialled"}));
  EXPECT_EQ(sink.lines(LogLevel::warn), (std::vector<std::string>{"slow peer"}));
  EXPECT_EQ(sink.text(), "net: slow peer\nnet.client: dialled\n");
  EXPECT_EQ(sink.count(LogLevel::info, "dial"), 1u);
}

}  // namespace
}  // namespace dannan::status
