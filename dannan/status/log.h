//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <iosfwd>
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
// compared side by side. `out` is for tests.
class ConsoleSink final : public LogSink {
 public:
  explicit ConsoleSink(LogLevel level = LogLevel::info, std::ostream* out = nullptr) : _level(level), _out(out) {}
  bool enabled(LogLevel level) const noexcept override;
  void write(LogLevel, std::string_view scope, std::string_view message) override;

 private:
  const LogLevel _level;
  std::ostream* const _out;
  // Held across the write.
  contract::IoMutex _mutex;
};

// stderr, one JSON object per line, for a log shipper:
// {"time":"2026-11-03T18:40:01Z","level":"WARN","scope":"net","message":"..."}.
// The same UTC time and filtering as ConsoleSink; the message escaped as
// JSON requires, control characters included. Every node of the stack runs
// under systemd and is shipped alike, so this is the stack's, not one
// project's. `out` is for tests.
class JsonSink final : public LogSink {
 public:
  explicit JsonSink(LogLevel level = LogLevel::info, std::ostream* out = nullptr) : _level(level), _out(out) {}
  bool enabled(LogLevel level) const noexcept override;
  void write(LogLevel, std::string_view scope, std::string_view message) override;

 private:
  const LogLevel _level;
  std::ostream* const _out;
  contract::IoMutex _mutex;
};

// A sink that takes nothing, for a component given none.
std::shared_ptr<LogSink> null_sink();

// How a node writes its log: "text" (ConsoleSink) or "json" (JsonSink), in
// any case. Throws std::invalid_argument naming the accepted spellings.
enum class LogFormat : unsigned char { text, json };
LogFormat parse_log_format(std::string_view);
std::shared_ptr<LogSink> console_sink(LogFormat format, LogLevel level);

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
