//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/status/log.h"

namespace dannan::test {

// Keeps every record, at every level, with its scope, for a test to look through. The stack's
// one capture sink (TODO/2026-11-20-stack-plan.md, items 17 and 28): Dagda's and Lugh's copies
// move onto it.
class RecordingSink final : public status::LogSink {
 public:
  bool enabled(status::LogLevel) const noexcept override { return true; }
  void write(status::LogLevel level, std::string_view scope, std::string_view message) override {
    contract::Lock lock(_mutex);
    _records.push_back({level, std::string(scope), std::string(message)});
  }

  // How many records at `level` contain `text`.
  size_t count(status::LogLevel level, std::string_view text) const {
    contract::Lock lock(_mutex);
    size_t found = 0;
    for (const auto& record : _records)
      if (record.level == level && record.message.find(text) != std::string::npos) ++found;
    return found;
  }

  // Every message, in order.
  std::vector<std::string> lines() const {
    contract::Lock lock(_mutex);
    std::vector<std::string> found;
    for (const auto& record : _records) found.push_back(record.message);
    return found;
  }

  // Every message at `level`, in order.
  std::vector<std::string> lines(status::LogLevel level) const {
    contract::Lock lock(_mutex);
    std::vector<std::string> found;
    for (const auto& record : _records)
      if (record.level == level) found.push_back(record.message);
    return found;
  }

  // The whole log as one string, "scope: message" a line, for asking whether something was said
  // at all.
  std::string text() const {
    contract::Lock lock(_mutex);
    std::string all;
    for (const auto& record : _records) all += record.scope + ": " + record.message + "\n";
    return all;
  }

 private:
  struct Record {
    status::LogLevel level;
    std::string scope;
    std::string message;
  };

  mutable contract::Mutex _mutex;
  std::vector<Record> _records DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::test
