//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_map>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dannan::listen {

// Failed sign-ins, keyed as the hosted system chooses (per principal, per source address),
// answered with a growing wait rather than a lock, so someone who knows a user's name cannot
// lock them out on purpose: the most they can do is make each guess slower, theirs included.
// Ask before any costly check (Kvasir: before hashing). Held in memory, per node; summing it
// across a cluster is the hosted system's. Lugh's, moved here so Lugh and Kvasir link one copy
// (Kvasir's ask, 2026-11-16). Time is injected. Thread-safe.
class Backoff {
 public:
  struct Rules {
    // Failures before any wait.
    std::uint32_t free_failures = 5;
    // The first wait, doubled at each failure after it, up to `longest`.
    std::chrono::seconds first{1};
    std::chrono::seconds longest{std::chrono::minutes(15)};
    // A key with no failure for this long starts again.
    std::chrono::seconds forget_after{std::chrono::hours(1)};
  };

  // Above this many keys, forgotten ones are dropped: callers choose the keys, and a stream
  // of made-up names must not grow the map without bound.
  explicit Backoff(Rules rules, std::size_t prune_above = 100000) : _rules(rules), _prune_above(prune_above) {}

  // How long until `key` may try again; nothing when it may now.
  std::optional<std::chrono::seconds> wait(const std::string& key, contract::TimePoint now) const {
    contract::Lock lock(_mutex);
    const auto found = _keys.find(key);
    if (found == _keys.end() || forgotten(found->second, now)) return std::nullopt;
    const auto until = found->second.last + delay(found->second.failures);
    if (now >= until) return std::nullopt;
    return std::max(std::chrono::seconds(1), std::chrono::ceil<std::chrono::seconds>(until - now));
  }

  void failed(const std::string& key, contract::TimePoint now) {
    contract::Lock lock(_mutex);
    if (_keys.size() > _prune_above) prune(now);
    auto& state = _keys[key];
    if (forgotten(state, now)) state.failures = 0;
    ++state.failures;
    state.last = now;
  }

  void succeeded(const std::string& key) {
    contract::Lock lock(_mutex);
    _keys.erase(key);
  }

  std::size_t size() const {
    contract::Lock lock(_mutex);
    return _keys.size();
  }

 private:
  struct State {
    std::uint32_t failures = 0;
    contract::TimePoint last{};
  };

  bool forgotten(const State& state, contract::TimePoint now) const { return state.failures == 0 || now - state.last >= _rules.forget_after; }

  std::chrono::seconds delay(std::uint32_t failures) const {
    if (failures < _rules.free_failures) return std::chrono::seconds(0);
    auto out = _rules.first;
    for (std::uint32_t i = _rules.free_failures; i < failures && out < _rules.longest; ++i) out *= 2;
    return std::min(out, _rules.longest);
  }

  void prune(contract::TimePoint now) DANNAN_REQUIRES(_mutex) {
    for (auto it = _keys.begin(); it != _keys.end();) it = forgotten(it->second, now) ? _keys.erase(it) : std::next(it);
  }

  const Rules _rules;
  const std::size_t _prune_above;
  mutable contract::Mutex _mutex;
  std::unordered_map<std::string, State> _keys DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::listen
