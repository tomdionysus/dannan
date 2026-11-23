//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// Failed sign-ins answered with a growing wait rather than a lock.

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

/// Failed sign-ins, answered with a growing wait rather than a lock (Lugh's, moved here so Lugh and
/// Kvasir link one copy).
///
/// Why a wait and not a lock: someone who knows a user's name must not be able to lock them out on
/// purpose. The most they can do is make each guess slower, theirs included.
///
/// Keyed as the hosted system chooses: per principal, per source address, or both. Ask wait()
/// before any costly check (before hashing a password, Kvasir), and report the result with
/// failed() or succeeded(). Held in memory, per node; summing across a cluster is the hosted
/// system's. Time is injected. Thread-safe; waits on nothing but its own lock.
///
/// The bound (0.12.0): callers choose the keys, so a stream of made-up names must not grow the
/// map. It holds at most `max_keys`. At the bound, forgotten keys go first, then those with the
/// fewest failures, the oldest of them first: a flood of names that each fail once never evicts a
/// name under a real attack, which has failed many times.
///
/// @code
/// listen::Backoff backoff(listen::Backoff::Rules{});
/// const auto key = account + "|" + address;
/// if (const auto wait = backoff.wait(key, time.now())) return refuse_for(*wait);
/// if (identity::verify_password(given, stored)) backoff.succeeded(key);
/// else backoff.failed(key, time.now());
/// @endcode
class Backoff {
 public:
  /// How the wait grows.
  struct Rules {
    /// Failures before any wait.
    std::uint32_t free_failures = 5;
    /// The first wait, doubled at each failure after it, up to `longest`.
    std::chrono::seconds first{1};
    /// The longest wait.
    std::chrono::seconds longest{std::chrono::minutes(15)};
    /// A key with no failure for this long starts again from nothing.
    std::chrono::seconds forget_after{std::chrono::hours(1)};
  };

  /// @param rules how the wait grows.
  /// @param max_keys the most keys held at once, at least one.
  explicit Backoff(Rules rules, std::size_t max_keys = 100000) : _rules(rules), _max_keys(std::max<std::size_t>(max_keys, 1)) {}

  /// How long until `key` may try again.
  /// @param key the key, as failed() was given it.
  /// @param now the time, from the caller's TimeSource.
  /// @return nothing when it may try now; otherwise the wait, at least one second.
  std::optional<std::chrono::seconds> wait(const std::string& key, contract::TimePoint now) const {
    contract::Lock lock(_mutex);
    const auto found = _keys.find(key);
    if (found == _keys.end() || forgotten(found->second, now)) return std::nullopt;
    const auto until = found->second.last + delay(found->second.failures);
    if (now >= until) return std::nullopt;
    return std::max(std::chrono::seconds(1), std::chrono::ceil<std::chrono::seconds>(until - now));
  }

  /// Counts a failure for `key` at `now`; a key forgotten (no failure for forget_after) starts
  /// again from nothing.
  void failed(const std::string& key, contract::TimePoint now) {
    contract::Lock lock(_mutex);
    if (!_keys.contains(key) && _keys.size() >= _max_keys) make_room(now);
    auto& state = _keys[key];
    if (forgotten(state, now)) state.failures = 0;
    ++state.failures;
    state.last = now;
  }

  /// Forgets `key`'s failures: a success clears its wait.
  void succeeded(const std::string& key) {
    contract::Lock lock(_mutex);
    _keys.erase(key);
  }

  /// How many keys are held now: at most the bound.
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
