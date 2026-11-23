//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dannan::listen {

// Token buckets keyed by what a caller is limited as: a client address, a session, an account
// (TODO/2026-11-20-stack-plan.md, item 10; Lugh's, moved here so Lugh, Mimir, Kvasir and Macha link
// one copy). A key gets `burst` requests at once, then `per_minute` a minute; a refusal says how
// long to wait, for Retry-After. Held in memory, per node; time is injected. Thread-safe, waits on
// nothing but its own lock.
//
// Callers choose the keys, so the map has a hard bound. At it, full buckets go first (one holds
// nothing a new key would not), then the keys touched longest ago. A flood of made-up keys can
// therefore evict a real key's partly spent bucket, giving it a fresh burst: limit by name and
// address together, and keep a global bucket for the listener, so no one key carries the limit.
class RateLimiter {
 public:
  struct Policy {
    uint32_t burst = 0;
    uint32_t per_minute = 0;
    // Zero in either means no limit.
    bool unlimited() const noexcept { return burst == 0 || per_minute == 0; }
  };

  explicit RateLimiter(size_t max_keys = 100000);

  // Takes one request from `key`'s bucket: nothing when it is allowed, else the wait until it
  // would be, rounded up to at least a second. An unlimited policy keeps no key. A key's bucket
  // follows the policy it is last given.
  std::optional<std::chrono::seconds> take(const std::string& key, const Policy& policy, contract::TimePoint now);

  size_t size() const;
  size_t max_keys() const noexcept { return _max_keys; }

 private:
  struct Bucket {
    double tokens = 0;
    contract::TimePoint at{};
    Policy policy;
    uint64_t touched = 0;
  };
  static void refill(Bucket& bucket, contract::TimePoint now);
  void make_room(contract::TimePoint now) DANNAN_REQUIRES(_mutex);

  const size_t _max_keys;
  mutable contract::Mutex _mutex;
  std::unordered_map<std::string, Bucket> _buckets DANNAN_GUARDED_BY(_mutex);
  uint64_t _touches DANNAN_GUARDED_BY(_mutex) = 0;
};

}  // namespace dannan::listen
