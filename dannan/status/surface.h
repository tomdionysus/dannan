//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>

#include "dannan/contract/thread_safety.h"

namespace dannan::status {

// The node's Status surface: what each hosted system publishes, by key, and
// its counters, read back as one snapshot for the Status page. Keys are the
// publisher's; prefixing them with the system's name keeps them apart.
// Thread-safe; nothing here waits.
class Surface {
 public:
  using Value = std::variant<bool, int64_t, uint64_t, double, std::string>;

  struct Snapshot {
    std::map<std::string, Value> values;
    std::map<std::string, uint64_t> counters;
  };

  void publish(const std::string& key, Value value);
  void withdraw(const std::string& key);
  // The counter by this name, made at zero on first use; the reference
  // stays valid for the surface's life, so a hot path keeps it.
  std::atomic<uint64_t>& counter(const std::string& name);
  Snapshot snapshot() const;

 private:
  mutable contract::Mutex _mutex;
  std::map<std::string, Value> _values DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, std::unique_ptr<std::atomic<uint64_t>>> _counters DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::status
