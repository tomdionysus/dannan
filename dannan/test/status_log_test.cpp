//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Log levels and scoped loggers: the inventory's row "log levels, scoped
// prefixes"; each component logs through its own scope.

#include <gtest/gtest.h>

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

}  // namespace
}  // namespace dannan::status
