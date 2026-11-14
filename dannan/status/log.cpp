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

std::tm utc_at(std::chrono::system_clock::time_point at) {
  const auto time = std::chrono::system_clock::to_time_t(at);
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

void ConsoleSink::write(LogLevel level, std::string_view scope, std::string_view message) { write_at(std::chrono::system_clock::now(), level, scope, message); }

void ConsoleSink::write_at(std::chrono::system_clock::time_point at, LogLevel level, std::string_view scope, std::string_view message) {
  const auto tm = utc_at(at);
  std::ostringstream line;
  line << std::put_time(&tm, "%F %TZ") << ' ' << log_level_name(level) << ' ' << scope << ": " << message << '\n';
  emit(_mutex, _out, line.str());
}

bool JsonSink::enabled(LogLevel level) const noexcept { return static_cast<int>(level) >= static_cast<int>(_level); }

void JsonSink::write(LogLevel level, std::string_view scope, std::string_view message) { write_at(std::chrono::system_clock::now(), level, scope, message); }

void JsonSink::write_at(std::chrono::system_clock::time_point at, LogLevel level, std::string_view scope, std::string_view message) {
  const auto tm = utc_at(at);
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

AsyncSink::AsyncSink(std::shared_ptr<LogSink> inner, size_t capacity) : _inner(std::move(inner)), _capacity(capacity) {
  if (!_inner) throw std::invalid_argument("AsyncSink: an inner sink is required");
  if (_capacity == 0) throw std::invalid_argument("AsyncSink: the queue must hold at least one record");
  _thread = std::thread([this] { run(); });
}

AsyncSink::~AsyncSink() {
  {
    contract::Lock lock(_mutex);
    _stopping = true;
  }
  _wake.notify_all();
  _thread.join();
}

void AsyncSink::write(LogLevel level, std::string_view scope, std::string_view message) { write_at(std::chrono::system_clock::now(), level, scope, message); }

void AsyncSink::write_at(std::chrono::system_clock::time_point at, LogLevel level, std::string_view scope, std::string_view message) {
  {
    contract::Lock lock(_mutex);
    if (_queue.size() >= _capacity) {
      ++_dropped;
      ++_unreported;
      return;
    }
    _queue.push_back({at, level, std::string(scope), std::string(message)});
    ++_queued;
  }
  _wake.notify_one();
}

uint64_t AsyncSink::dropped() const {
  contract::Lock lock(_mutex);
  return _dropped;
}

void AsyncSink::flush() {
  contract::Lock lock(_mutex);
  const auto target = _queued;
  _written.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return (_done >= target && _unreported == 0) || _stopping; });
}

void AsyncSink::run() {
  for (;;) {
    std::deque<Record> batch;
    uint64_t unreported = 0;
    {
      contract::Lock lock(_mutex);
      _wake.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return !_queue.empty() || _unreported > 0 || _stopping; });
      if (_queue.empty() && _unreported == 0) return;  // stopping, with nothing left
      batch.swap(_queue);
      unreported = _unreported;
      _unreported = 0;
    }
    // Outside the lock: the inner sink may wait on its device, and writers never wait on it.
    for (const auto& record : batch) {
      try {
        _inner->write_at(record.at, record.level, record.scope, record.message);
      } catch (...) {
        // A record the inner sink could not take is lost, as a dropped one would be.
      }
    }
    if (unreported > 0 && _inner->enabled(LogLevel::warn)) {
      try {
        _inner->write_at(std::chrono::system_clock::now(), LogLevel::warn, "log",
                         "dropped " + std::to_string(unreported) + " record(s): the log queue was full");
      } catch (...) {
      }
    }
    {
      contract::Lock lock(_mutex);
      _done += batch.size();
    }
    _written.notify_all();
  }
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
