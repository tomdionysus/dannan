//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/status/log.h"

namespace dannan::test {

// Keeps every record, at every level, for a test to look through.
class RecordingSink final : public status::LogSink {
 public:
  bool enabled(status::LogLevel) const noexcept override { return true; }
  void write(status::LogLevel level, std::string_view, std::string_view message) override {
    std::lock_guard lock(_mutex);
    _records.emplace_back(level, std::string(message));
  }
  // How many records at `level` contain `text`.
  size_t count(status::LogLevel level, std::string_view text) const {
    std::lock_guard lock(_mutex);
    size_t found = 0;
    for (const auto& [at, message] : _records)
      if (at == level && message.find(text) != std::string::npos) ++found;
    return found;
  }

 private:
  mutable std::mutex _mutex;
  std::vector<std::pair<status::LogLevel, std::string>> _records;
};

}  // namespace dannan::test
