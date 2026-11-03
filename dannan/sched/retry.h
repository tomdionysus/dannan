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
#include <deque>
#include <optional>

#include "dannan/contract/time_source.h"

namespace dannan::sched {

// One retry discipline for every unit of work that can fail and try again:
// exponential backoff to a ceiling, a failure budget over a window, and a
// terminal parked outcome that hands the item to an operator instead of
// retrying for ever (discipline 2). The policy is data; the state is per item.
struct RetryPolicy {
  // More than this many failures inside `failure_window` parks the item.
  size_t max_failures_in_window = 5;
  std::chrono::milliseconds failure_window = std::chrono::minutes(10);
  std::chrono::milliseconds initial_backoff = std::chrono::seconds(1);
  std::chrono::milliseconds max_backoff = std::chrono::seconds(60);
  // Backstop for what the window cannot see: an item failing unbroken this
  // long parks however sparsely retried. Zero disables it.
  std::chrono::milliseconds max_failing_duration = std::chrono::hours(1);
};

// One item's retry state. Time comes from the caller's injected source.
// Single owner.
class RetryState {
 public:
  using TimePoint = contract::TimePoint;

  // Records a failure at `now`. The delay before the next attempt, or none
  // when the budget is spent and the item must be parked.
  std::optional<std::chrono::milliseconds> failed(const RetryPolicy& policy, TimePoint now) {
    if (_consecutive == 0) _failing_since = now;
    ++_consecutive;
    ++_total;
    if (_first_failure == TimePoint{}) _first_failure = now;
    _last_failure = now;
    _recent.push_back(now);
    while (!_recent.empty() && now - _recent.front() > policy.failure_window) _recent.pop_front();
    if (_recent.size() > policy.max_failures_in_window) return std::nullopt;
    if (policy.max_failing_duration.count() > 0 && now - _failing_since > policy.max_failing_duration) return std::nullopt;
    _backoff = _backoff == std::chrono::milliseconds{} ? policy.initial_backoff : std::min(policy.max_backoff, _backoff * 2);
    _due = now + _backoff;
    return _backoff;
  }

  // A clean attempt resets the backoff and the run; the window is kept, so a
  // flapping item still parks.
  void succeeded() {
    _consecutive = 0;
    _failing_since = {};
    _backoff = {};
    _due = {};
  }

  // Operator action: forget everything and allow an immediate attempt.
  void reset() { *this = RetryState{}; }

  bool due(TimePoint now) const noexcept { return now >= _due; }
  TimePoint due_at() const noexcept { return _due; }
  size_t consecutive_failures() const noexcept { return _consecutive; }
  size_t total_failures() const noexcept { return _total; }
  size_t failures_in_window() const noexcept { return _recent.size(); }
  TimePoint first_failure() const noexcept { return _first_failure; }
  TimePoint last_failure() const noexcept { return _last_failure; }
  // Start of the current unbroken run of failures; unset when not failing.
  TimePoint failing_since() const noexcept { return _failing_since; }
  std::chrono::milliseconds current_backoff() const noexcept { return _backoff; }

 private:
  std::deque<TimePoint> _recent;
  std::chrono::milliseconds _backoff{};
  TimePoint _due{};
  TimePoint _first_failure{};
  TimePoint _last_failure{};
  TimePoint _failing_since{};
  size_t _consecutive = 0;
  size_t _total = 0;
};

}  // namespace dannan::sched
