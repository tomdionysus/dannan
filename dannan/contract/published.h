//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <memory>
#include <stdexcept>
#include <utility>

#include "dannan/contract/thread_safety.h"

namespace dannan::contract {

// Published immutable state: an immutable snapshot behind an owning pointer
// that a publish swaps. A publish never changes what a held handle reads; the
// old snapshot lives until its last holder lets go.
template <class T>
class Published {
 public:
  using Handle = std::shared_ptr<const T>;

  Published() = default;
  explicit Published(Handle initial) { publish(std::move(initial)); }
  Published(const Published&) = delete;
  Published& operator=(const Published&) = delete;

  // The current snapshot, or null before the first publish. Thread-safe;
  // waits only on the pointer copy.
  Handle handle() const {
    Lock lock(_mutex);
    return _current;
  }

  // Replaces the current snapshot; a published state is never withdrawn.
  // Thread-safe. A snapshot this held the last reference to is destroyed
  // after the lock is released, so a reader never waits on a destructor.
  void publish(Handle next) {
    if (!next) throw std::invalid_argument("Published::publish: a snapshot is required");
    {
      Lock lock(_mutex);
      _current.swap(next);
    }
  }
  void publish(T value) { publish(std::make_shared<const T>(std::move(value))); }

 private:
  mutable Mutex _mutex;
  Handle _current DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::contract
