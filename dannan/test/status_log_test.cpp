//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Log levels and scoped loggers: the inventory's row "log levels, scoped
// prefixes"; each component logs through its own scope.

#include <gtest/gtest.h>

#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "dannan/status/log.h"

namespace dannan::status {
namespace {

class Recording final : public LogSink {
 public:
  explicit Recording(LogLevel floor) : floor(floor) {}
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

TEST(LogFormat, ParsesTextAndJsonInAnyCase) {
  EXPECT_EQ(parse_log_format("text"), LogFormat::text);
  EXPECT_EQ(parse_log_format("JSON"), LogFormat::json);
  EXPECT_THROW(parse_log_format("xml"), std::invalid_argument);
  EXPECT_NE(dynamic_cast<JsonSink*>(console_sink(LogFormat::json, LogLevel::info).get()), nullptr);
  EXPECT_NE(dynamic_cast<ConsoleSink*>(console_sink(LogFormat::text, LogLevel::info).get()), nullptr);
}

}  // namespace
}  // namespace dannan::status
