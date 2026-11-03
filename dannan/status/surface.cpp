//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/surface.h"

namespace dannan::status {

void Surface::publish(const std::string& key, Value value) {
  contract::Lock lock(_mutex);
  _values[key] = std::move(value);
}

void Surface::withdraw(const std::string& key) {
  contract::Lock lock(_mutex);
  _values.erase(key);
}

std::atomic<uint64_t>& Surface::counter(const std::string& name) {
  contract::Lock lock(_mutex);
  auto& slot = _counters[name];
  if (!slot) slot = std::make_unique<std::atomic<uint64_t>>(0);
  return *slot;
}

Surface::Snapshot Surface::snapshot() const {
  contract::Lock lock(_mutex);
  Snapshot out;
  out.values = _values;
  for (const auto& [name, count] : _counters) out.counters.emplace(name, count->load(std::memory_order_relaxed));
  return out;
}

}  // namespace dannan::status
