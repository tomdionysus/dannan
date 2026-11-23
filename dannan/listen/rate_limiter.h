//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// Token buckets keyed by whatever a caller is limited as, with a hard bound on the keys.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dannan::listen {

/// Token buckets keyed by what a caller is limited as: a client address, a session, an account
/// (0.12.0; Lugh's, moved here so Lugh, Mimir, Kvasir and Macha link one copy).
///
/// A key gets `burst` requests at once, then `per_minute` a minute; a refusal says how long to
/// wait, for a Retry-After header. Held in memory, per node, so limiting needs no store round
/// trip; summing across a cluster is the hosted system's.
///
/// The bound: callers choose the keys, so a flood of made-up ones must not grow the map. It holds
/// at most max_keys(). At the bound, full buckets go first, since a full bucket holds nothing a new
/// key would not; then the keys touched longest ago. A flood can therefore evict a real key's
/// partly spent bucket, giving it a fresh burst, so limit by name and address together and keep
/// a global bucket for the listener: then no one key carries the limit alone. Freeing an eighth of
/// the bound at a time keeps a flood's cost constant per key.
///
/// Time is injected (the `now` each call is given). Thread-safe; waits on nothing but its own lock.
///
/// @code
/// listen::RateLimiter limits;
/// const listen::RateLimiter::Policy per_address{30, 30};  // 30 at once, then 30 a minute
/// if (const auto wait = limits.take(client_address, per_address, time.now()))
///   return respond_429(*wait);  // Retry-After: wait->count()
/// @endcode
class RateLimiter {
 public:
  /// How much a key may take.
  struct Policy {
    /// Requests allowed at once.
    uint32_t burst = 0;
    /// Requests a minute after the burst.
    uint32_t per_minute = 0;
    /// Zero in either means no limit.
    bool unlimited() const noexcept { return burst == 0 || per_minute == 0; }
  };

  /// @param max_keys the most keys held at once, at least one.
  explicit RateLimiter(size_t max_keys = 100000);

  /// Takes one request from `key`'s bucket.
  /// @param key what the caller is limited as; a new key starts with a full bucket.
  /// @param policy the key's limit; a key's bucket follows the policy it is last given, and an
  ///   unlimited one keeps no key at all.
  /// @param now the time, from the caller's TimeSource; a time before the bucket's last moves
  ///   nothing.
  /// @return nothing when the request is allowed; otherwise how long until it would be, rounded up
  ///   to at least one second.
  std::optional<std::chrono::seconds> take(const std::string& key, const Policy& policy, contract::TimePoint now);

  /// How many keys are held now.
  size_t size() const;
  /// The bound on size().
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
