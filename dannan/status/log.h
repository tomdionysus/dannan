//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"

namespace dannan::status {

enum class LogLevel : unsigned char { all = 0, debug = 1, info = 2, warn = 3, error = 4 };

// "ALL", "DEBUG", "INFO", "WARN", "ERROR".
std::string_view log_level_name(LogLevel) noexcept;
// Case-insensitive; WARNING is accepted for WARN. Throws std::invalid_argument
// naming the accepted spellings.
LogLevel parse_log_level(std::string_view);

// Where records go. One per node, handed to every component; there is no
// process-wide logger. enabled() is the filter; write() writes an approved
// record and must not filter again.
class LogSink {
 public:
  virtual ~LogSink() = default;
  virtual bool enabled(LogLevel) const noexcept = 0;
  // May wait on the sink's device; thread-safe.
  virtual void write(LogLevel, std::string_view scope, std::string_view message) = 0;
};

// stderr, one line per record: "2026-11-03 18:40:01Z WARN scope: message".
// UTC, because nodes run in different timezones and their journals are
// compared side by side.
class ConsoleSink final : public LogSink {
 public:
  explicit ConsoleSink(LogLevel level = LogLevel::info) : _level(level) {}
  bool enabled(LogLevel level) const noexcept override;
  void write(LogLevel, std::string_view scope, std::string_view message) override;

 private:
  const LogLevel _level;
  // Held across the write to stderr.
  contract::IoMutex _mutex;
};

// A component's logger: the node's sink and the component's short scope
// name, so every line says which component wrote it. Cheap to copy.
class ScopedLogger {
 public:
  ScopedLogger(std::shared_ptr<LogSink> sink, std::string scope);

  bool enabled(LogLevel level) const noexcept { return _sink->enabled(level); }
  void trace(std::string_view message) const { log(LogLevel::all, message); }
  void debug(std::string_view message) const { log(LogLevel::debug, message); }
  void info(std::string_view message) const { log(LogLevel::info, message); }
  void warn(std::string_view message) const { log(LogLevel::warn, message); }
  void error(std::string_view message) const { log(LogLevel::error, message); }
  void log(LogLevel level, std::string_view message) const {
    if (_sink->enabled(level)) _sink->write(level, _scope, message);
  }

  const std::string& scope() const noexcept { return _scope; }
  // A logger for a part of this component: "net" becomes "net.client".
  ScopedLogger child(std::string_view part) const;

 private:
  std::shared_ptr<LogSink> _sink;
  std::string _scope;
};

}  // namespace dannan::status
