//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/rate_limiter.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>
#include <vector>

namespace dannan::listen {

RateLimiter::RateLimiter(size_t max_keys) : _max_keys(std::max<size_t>(max_keys, 1)) {}

void RateLimiter::refill(Bucket& bucket, contract::TimePoint now) {
  if (now <= bucket.at) return;
  const auto elapsed = std::chrono::duration<double>(now - bucket.at).count();
  bucket.tokens = std::min<double>(bucket.policy.burst, bucket.tokens + elapsed * bucket.policy.per_minute / 60.0);
  bucket.at = now;
}

void RateLimiter::make_room(contract::TimePoint now) {
  // An eighth of the bound freed at a time, so a flood of new keys costs a sweep per eighth, not
  // one per key.
  const size_t target = _max_keys - std::max<size_t>(_max_keys / 8, 1);
  for (auto it = _buckets.begin(); it != _buckets.end();) {
    refill(it->second, now);
    it = it->second.tokens >= it->second.policy.burst ? _buckets.erase(it) : std::next(it);
  }
  if (_buckets.size() <= target) return;
  std::vector<std::pair<uint64_t, const std::string*>> order;
  order.reserve(_buckets.size());
  for (const auto& [key, bucket] : _buckets) order.emplace_back(bucket.touched, &key);
  const auto evict = _buckets.size() - target;
  std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(evict - 1), order.end());
  std::vector<std::string> gone;
  gone.reserve(evict);
  for (size_t i = 0; i < evict; ++i) gone.push_back(*order[i].second);
  for (const auto& key : gone) _buckets.erase(key);
}

std::optional<std::chrono::seconds> RateLimiter::take(const std::string& key, const Policy& policy, contract::TimePoint now) {
  if (policy.unlimited()) return std::nullopt;
  contract::Lock lock(_mutex);
  auto found = _buckets.find(key);
  if (found == _buckets.end()) {
    if (_buckets.size() >= _max_keys) make_room(now);
    found = _buckets.emplace(key, Bucket{static_cast<double>(policy.burst), now, policy, 0}).first;
  } else {
    refill(found->second, now);
    found->second.policy = policy;
    found->second.tokens = std::min<double>(found->second.tokens, policy.burst);
  }
  auto& bucket = found->second;
  bucket.touched = ++_touches;
  if (bucket.tokens >= 1.0) {
    bucket.tokens -= 1.0;
    return std::nullopt;
  }
  // Less a nanosecond, so a wait of exactly 20 s computed as 20.000000000000004 says 20, not 21.
  const auto wait = std::ceil((1.0 - bucket.tokens) * 60.0 / policy.per_minute - 1e-9);
  return std::chrono::seconds(std::max<int64_t>(1, static_cast<int64_t>(wait)));
}

size_t RateLimiter::size() const {
  contract::Lock lock(_mutex);
  return _buckets.size();
}

}  // namespace dannan::listen
