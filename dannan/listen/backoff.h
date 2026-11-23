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
#include <utility>
#include <vector>

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

  // At most this many keys (0.12.0: a hard bound; item 10, Kvasir's proof). Callers choose the
  // keys, so a stream of made-up names must not grow the map. At the bound, forgotten keys go
  // first, then those with the fewest failures, the oldest of them first: a flood of names that
  // fail once each never evicts a name under a real attack, which has failed many times.
  explicit Backoff(Rules rules, std::size_t max_keys = 100000) : _rules(rules), _max_keys(std::max<std::size_t>(max_keys, 1)) {}

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
    if (!_keys.contains(key) && _keys.size() >= _max_keys) make_room(now);
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

  // An eighth of the bound freed at a time, so a flood costs a sweep per eighth, not per key.
  void make_room(contract::TimePoint now) DANNAN_REQUIRES(_mutex) {
    const std::size_t target = _max_keys - std::max<std::size_t>(_max_keys / 8, 1);
    for (auto it = _keys.begin(); it != _keys.end();) it = forgotten(it->second, now) ? _keys.erase(it) : std::next(it);
    if (_keys.size() <= target) return;
    std::vector<std::pair<const State*, const std::string*>> order;
    order.reserve(_keys.size());
    for (const auto& [name, state] : _keys) order.emplace_back(&state, &name);
    const auto evict = _keys.size() - target;
    std::nth_element(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(evict - 1), order.end(), [](const auto& a, const auto& b) {
      return a.first->failures != b.first->failures ? a.first->failures < b.first->failures : a.first->last < b.first->last;
    });
    std::vector<std::string> gone;
    gone.reserve(evict);
    for (std::size_t i = 0; i < evict; ++i) gone.push_back(*order[i].second);
    for (const auto& name : gone) _keys.erase(name);
  }

  const Rules _rules;
  const std::size_t _max_keys;
  mutable contract::Mutex _mutex;
  std::unordered_map<std::string, State> _keys DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::listen
