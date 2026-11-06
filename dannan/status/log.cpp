//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/log.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
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

namespace {

// One record, whole. Every node in a process may have a sink on stderr, so stderr gets
// one write and no shared formatting state; a test's stream is only that sink's.
void emit(contract::IoMutex& mutex, std::ostream* out, const std::string& line) {
  if (out) {
    contract::Lock lock(mutex);
    *out << line;
    return;
  }
  size_t written = 0;
  while (written < line.size()) {
    const auto wrote = ::write(STDERR_FILENO, line.data() + written, line.size() - written);
    if (wrote <= 0) return;
    written += static_cast<size_t>(wrote);
  }
}

}  // namespace

bool ConsoleSink::enabled(LogLevel level) const noexcept { return static_cast<int>(level) >= static_cast<int>(_level); }

namespace {

std::tm utc_now() {
  const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  gmtime_r(&time, &tm);
  return tm;
}

void json_string(std::ostream& out, std::string_view text) {
  out << '"';
  for (const char c : text) {
    switch (c) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
          out << escaped;
        } else {
          out << c;
        }
    }
  }
  out << '"';
}

}  // namespace

void ConsoleSink::write(LogLevel level, std::string_view scope, std::string_view message) {
  const auto tm = utc_now();
  std::ostringstream line;
  line << std::put_time(&tm, "%F %TZ") << ' ' << log_level_name(level) << ' ' << scope << ": " << message << '\n';
  emit(_mutex, _out, line.str());
}

bool JsonSink::enabled(LogLevel level) const noexcept { return static_cast<int>(level) >= static_cast<int>(_level); }

void JsonSink::write(LogLevel level, std::string_view scope, std::string_view message) {
  const auto tm = utc_now();
  std::ostringstream out;
  out << "{\"time\":\"" << std::put_time(&tm, "%FT%TZ") << "\",\"level\":\"" << log_level_name(level) << "\",\"scope\":";
  json_string(out, scope);
  out << ",\"message\":";
  json_string(out, message);
  out << "}\n";
  emit(_mutex, _out, out.str());
}

LogFormat parse_log_format(std::string_view value) {
  std::string format(value);
  std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (format == "text") return LogFormat::text;
  if (format == "json") return LogFormat::json;
  throw std::invalid_argument("invalid log format: " + std::string(value) + " (expected text or json)");
}

std::shared_ptr<LogSink> null_sink() {
  class Null final : public LogSink {
   public:
    bool enabled(LogLevel) const noexcept override { return false; }
    void write(LogLevel, std::string_view, std::string_view) override {}
  };
  static const auto sink = std::make_shared<Null>();
  return sink;
}

std::shared_ptr<LogSink> console_sink(LogFormat format, LogLevel level) {
  if (format == LogFormat::json) return std::make_shared<JsonSink>(level);
  return std::make_shared<ConsoleSink>(level);
}

ScopedLogger::ScopedLogger(std::shared_ptr<LogSink> sink, std::string scope) : _sink(std::move(sink)), _scope(std::move(scope)) {
  if (!_sink) throw std::invalid_argument("ScopedLogger: a sink is required");
}

ScopedLogger ScopedLogger::child(std::string_view part) const { return ScopedLogger(_sink, _scope + "." + std::string(part)); }

}  // namespace dannan::status
