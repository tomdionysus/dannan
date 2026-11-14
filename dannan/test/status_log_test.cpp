//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Log levels and scoped loggers: the inventory's row "log levels, scoped
// prefixes"; each component logs through its own scope.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "dannan/status/log.h"
#include "dannan/test/fixtures/gate.h"

namespace dannan::status {
namespace {

class Recording final : public LogSink {
 public:
  explicit Recording(LogLevel lowest) : floor(lowest) {}
  bool enabled(LogLevel level) const noexcept override { return static_cast<int>(level) >= static_cast<int>(floor); }
  void write(LogLevel level, std::string_view scope, std::string_view message) override {
    lines.push_back(std::string(log_level_name(level)) + " " + std::string(scope) + ": " + std::string(message));
  }
  LogLevel floor;
  std::vector<std::string> lines;
};

TEST(LogLevel, ParsesTheDocumentedSpellingsInAnyCase) {
  EXPECT_EQ(parse_log_level("all"), LogLevel::all);
  EXPECT_EQ(parse_log_level("Debug"), LogLevel::debug);
  EXPECT_EQ(parse_log_level("INFO"), LogLevel::info);
  EXPECT_EQ(parse_log_level("warning"), LogLevel::warn);
  EXPECT_EQ(parse_log_level("WARN"), LogLevel::warn);
  EXPECT_EQ(parse_log_level("error"), LogLevel::error);
  EXPECT_THROW(parse_log_level("verbose"), std::invalid_argument);
}

TEST(LogLevel, NamesRoundTrip) {
  for (const auto level : {LogLevel::all, LogLevel::debug, LogLevel::info, LogLevel::warn, LogLevel::error})
    EXPECT_EQ(parse_log_level(log_level_name(level)), level);
}

TEST(ScopedLogger, WritesOnlyWhatTheSinkEnablesUnderItsScope) {
  auto sink = std::make_shared<Recording>(LogLevel::info);
  const ScopedLogger logger(sink, "net");
  logger.debug("hidden");
  logger.info("dialled");
  logger.error("lost");
  ASSERT_EQ(sink->lines.size(), 2u);
  EXPECT_EQ(sink->lines[0], "INFO net: dialled");
  EXPECT_EQ(sink->lines[1], "ERROR net: lost");
}

TEST(ScopedLogger, AChildNamesThePartOfItsComponent) {
  auto sink = std::make_shared<Recording>(LogLevel::all);
  ScopedLogger(sink, "net").child("client").warn("x");
  ASSERT_EQ(sink->lines.size(), 1u);
  EXPECT_EQ(sink->lines[0], "WARN net.client: x");
}

TEST(ScopedLogger, RefusesToLogNowhere) { EXPECT_THROW(ScopedLogger(nullptr, "net"), std::invalid_argument); }

TEST(ConsoleSink, EnablesItsLevelAndAbove) {
  const ConsoleSink sink(LogLevel::warn);
  EXPECT_FALSE(sink.enabled(LogLevel::info));
  EXPECT_TRUE(sink.enabled(LogLevel::warn));
  EXPECT_TRUE(sink.enabled(LogLevel::error));
  EXPECT_TRUE(ConsoleSink(LogLevel::all).enabled(LogLevel::all));
}

// The text sink: UTC time, level, scope and message on one line.
TEST(ConsoleSink, WritesOneLinePerRecord) {
  std::ostringstream out;
  ConsoleSink sink(LogLevel::info, &out);
  ScopedLogger(std::shared_ptr<LogSink>(&sink, [](LogSink*) {}), "net").warn("a peer left");
  EXPECT_TRUE(std::regex_match(out.str(), std::regex(R"(\d{4}-\d\d-\d\d \d\d:\d\d:\d\dZ WARN net: a peer left\n)"))) << out.str();
}

// The JSON sink: one object per line for a shipper, the same time and
// filtering, the message escaped as JSON requires.
TEST(JsonSink, WritesOneObjectPerLineEscaped) {
  std::ostringstream out;
  auto sink = std::make_shared<JsonSink>(LogLevel::info, &out);
  const ScopedLogger logger(std::shared_ptr<LogSink>(sink.get(), [](LogSink*) {}), "lugh.oauth");
  logger.debug("not written");
  logger.error("a \"quoted\" \\ path\nand a tab\t and \x01");
  EXPECT_TRUE(std::regex_match(
      out.str(),
      std::regex(
          R"re(\{"time":"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ","level":"ERROR","scope":"lugh\.oauth","message":"a \\"quoted\\" \\\\ path\\nand a tab\\t and \\u0001"\}\n)re")))
      << out.str();
}

// An inner sink whose device is held shut until the test opens it.
class Held final : public LogSink {
 public:
  bool enabled(LogLevel level) const noexcept override { return level >= LogLevel::info; }
  void write(LogLevel level, std::string_view scope, std::string_view message) override { write_at(std::chrono::system_clock::now(), level, scope, message); }
  void write_at(std::chrono::system_clock::time_point at, LogLevel level, std::string_view scope, std::string_view message) override {
    gate.enter_and_wait();
    std::lock_guard lock(mutex);
    lines.push_back(std::string(log_level_name(level)) + " " + std::string(scope) + ": " + std::string(message));
    times.push_back(at);
  }
  std::vector<std::string> taken() {
    std::lock_guard lock(mutex);
    return lines;
  }
  std::vector<std::chrono::system_clock::time_point> stamped() {
    std::lock_guard lock(mutex);
    return times;
  }
  test::Gate gate;
  std::mutex mutex;
  std::vector<std::string> lines;
  std::vector<std::chrono::system_clock::time_point> times;
};

// The asynchronous stage (P6): a writer never waits on the inner sink's
// device; past the queue's bound records are dropped and counted, and the
// count is written through after what was queued, in order.
TEST(AsyncSink, NeverWaitsOnItsSinkAndReportsWhatItDropped) {
  auto inner = std::make_shared<Held>();
  AsyncSink sink(inner, 4);
  // Opened before the stage joins its thread, however the test leaves.
  test::GateOpener opener(inner->gate);
  EXPECT_TRUE(sink.enabled(LogLevel::info));
  EXPECT_FALSE(sink.enabled(LogLevel::debug));
  const ScopedLogger logger(std::shared_ptr<LogSink>(&sink, [](LogSink*) {}), "net");
  logger.info("record 0");
  // The stage's thread holds record 0 in the shut device: the queue is empty.
  ASSERT_TRUE(inner->gate.wait_for_entries(1));
  for (int i = 1; i <= 7; ++i) logger.info("record " + std::to_string(i));
  // Every write above returned with the device still shut.
  EXPECT_TRUE(inner->taken().empty());
  EXPECT_EQ(sink.dropped(), 3u);
  // Each record keeps the time it was written, not the time it went through.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto opened = std::chrono::system_clock::now();
  inner->gate.open();
  sink.flush();
  const auto times = inner->stamped();
  ASSERT_EQ(times.size(), 6u);
  for (size_t i = 0; i < 5; ++i) EXPECT_LT(times[i], opened) << i;
  const std::vector<std::string> expected{"INFO net: record 0", "INFO net: record 1", "INFO net: record 2",
                                          "INFO net: record 3", "INFO net: record 4", "WARN log: dropped 3 record(s): the log queue was full"};
  EXPECT_EQ(inner->taken(), expected);
  logger.warn("after");
  sink.flush();
  EXPECT_EQ(inner->taken().back(), "WARN net: after");
  EXPECT_EQ(sink.dropped(), 3u);
}

// Destruction writes what is still queued before it joins.
TEST(AsyncSink, WritesWhatIsQueuedWhenDestroyed) {
  auto inner = std::make_shared<Held>();
  inner->gate.open();
  {
    AsyncSink sink(inner, 64);
    for (int i = 0; i < 20; ++i) sink.write(LogLevel::info, "net", "record " + std::to_string(i));
  }
  const auto lines = inner->taken();
  ASSERT_EQ(lines.size(), 20u);
  EXPECT_EQ(lines.back(), "INFO net: record 19");
  EXPECT_THROW(AsyncSink(nullptr), std::invalid_argument);
  EXPECT_THROW(AsyncSink(inner, 0), std::invalid_argument);
}

TEST(LogFormat, ParsesTextAndJsonInAnyCase) {
  EXPECT_EQ(parse_log_format("text"), LogFormat::text);
  EXPECT_EQ(parse_log_format("JSON"), LogFormat::json);
  EXPECT_THROW(parse_log_format("xml"), std::invalid_argument);
  EXPECT_NE(dynamic_cast<JsonSink*>(console_sink(LogFormat::json, LogLevel::info).get()), nullptr);
  EXPECT_NE(dynamic_cast<ConsoleSink*>(console_sink(LogFormat::text, LogLevel::info).get()), nullptr);
}

}  // namespace
}  // namespace dannan::status
