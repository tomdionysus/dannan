//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace dannan::status {

std::string_view log_level_name(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::all:
      return "ALL";
    case LogLevel::debug:
      return "DEBUG";
    case LogLevel::info:
      return "INFO";
    case LogLevel::warn:
      return "WARN";
    case LogLevel::error:
      return "ERROR";
  }
  return "?";
}

LogLevel parse_log_level(std::string_view value) {
  std::string level(value);
  std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (level == "ALL") return LogLevel::all;
  if (level == "DEBUG") return LogLevel::debug;
  if (level == "INFO") return LogLevel::info;
  if (level == "WARN" || level == "WARNING") return LogLevel::warn;
  if (level == "ERROR") return LogLevel::error;
  throw std::invalid_argument("invalid log level: " + std::string(value) + " (expected ALL, DEBUG, INFO, WARN or ERROR)");
}

bool ConsoleSink::enabled(LogLevel level) const noexcept { return static_cast<int>(level) >= static_cast<int>(_level); }

void ConsoleSink::write(LogLevel level, std::string_view scope, std::string_view message) {
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&time, &tm);
  contract::Lock lock(_mutex);
  std::cerr << std::put_time(&tm, "%F %TZ") << ' ' << log_level_name(level) << ' ' << scope << ": " << message << '\n';
}

ScopedLogger::ScopedLogger(std::shared_ptr<LogSink> sink, std::string scope) : _sink(std::move(sink)), _scope(std::move(scope)) {
  if (!_sink) throw std::invalid_argument("ScopedLogger: a sink is required");
}

ScopedLogger ScopedLogger::child(std::string_view part) const { return ScopedLogger(_sink, _scope + "." + std::string(part)); }

}  // namespace dannan::status
