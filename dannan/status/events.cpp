//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/events.h"

#include <algorithm>
#include <stdexcept>

namespace dannan::status {

NodeEvents::Kind NodeEvents::kind(const std::string& name) {
  contract::Lock lock(_names_mutex);
  const auto found = std::find(_names.begin(), _names.end(), name);
  if (found != _names.end()) return static_cast<Kind>(found - _names.begin());
  if (_names.size() == max_kinds) throw std::length_error("too many node event kinds");
  _names.push_back(name);
  return _names.size() - 1;
}

void NodeEvents::notify(Kind kind) {
  _counts[kind].fetch_add(1, std::memory_order_release);
  {
    // Taken so a waiter between its check and its wait cannot miss this.
    contract::Lock lock(_wait_mutex);
  }
  _changed.notify_all();
}

uint64_t NodeEvents::total() const noexcept {
  uint64_t sum = 0;
  for (const auto& count : _counts) sum += count.load(std::memory_order_acquire);
  return sum;
}

bool NodeEvents::wait_for_change(uint64_t seen, std::chrono::milliseconds timeout) {
  contract::Lock lock(_wait_mutex);
  return _changed.wait_for(lock.native(), timeout, [&] { return total() != seen; });
}

}  // namespace dannan::status
