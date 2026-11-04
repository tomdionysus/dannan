//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/duty_share.h"

#include <algorithm>
#include <stdexcept>

namespace dannan::sched {

namespace {
// A rest is never longer than a day, whatever the weights.
constexpr auto longest_rest = std::chrono::milliseconds(std::chrono::hours(24));
}  // namespace

DutyShare::DutyShare(uint32_t foreground_weight, uint32_t background_weight, std::chrono::milliseconds slice)
    : _foreground_weight(foreground_weight), _background_weight(background_weight), _slice(slice) {
  if (!_foreground_weight || !_background_weight) throw std::invalid_argument("a duty share's weights are at least 1");
  if (_slice.count() <= 0) throw std::invalid_argument("a duty share's slice is positive");
}

void DutyShare::clear() {
  _burst.reset();
  _slice_ends = {};
  _rest_until = {};
}

void DutyShare::begin(contract::TimePoint now) {
  if (_burst) return;
  _burst = now;
  _slice_ends = now + _slice;
}

bool DutyShare::may_start(contract::TimePoint now, bool contended) {
  contract::Lock lock(_mutex);
  if (!contended) {
    clear();
    return true;
  }
  if (now < _rest_until) return false;
  return !_burst || now < _slice_ends;
}

void DutyShare::started(contract::TimePoint now, bool contended, bool serving) {
  contract::Lock lock(_mutex);
  ++_workers;
  if (!contended) return clear();
  if (serving) begin(now);
}

void DutyShare::serving(contract::TimePoint now, bool contended) {
  contract::Lock lock(_mutex);
  if (!contended) return clear();
  begin(now);
}

bool DutyShare::should_yield(contract::TimePoint now, bool contended) {
  contract::Lock lock(_mutex);
  if (!contended) {
    clear();
    return false;
  }
  if (!_burst) {
    // Contention arrived mid-burst: the burst ends here, and what it takes
    // to stop counts as the contended burst.
    _burst = now;
    _slice_ends = now;
    return true;
  }
  return now >= _slice_ends;
}

std::chrono::milliseconds DutyShare::finished(contract::TimePoint now, bool contended) {
  contract::Lock lock(_mutex);
  if (_workers) --_workers;
  if (!contended) {
    clear();
    return {};
  }
  if (_workers || !_burst) return {};
  const auto active = std::max(std::chrono::duration_cast<std::chrono::milliseconds>(now - *_burst), std::chrono::milliseconds(1));
  const auto rest = std::min(longest_rest, std::chrono::milliseconds(active.count() * _foreground_weight / _background_weight));
  _rest_until = now + rest;
  _burst.reset();
  _slice_ends = {};
  return rest;
}

std::chrono::milliseconds DutyShare::wait(contract::TimePoint now, bool contended) {
  contract::Lock lock(_mutex);
  if (!contended) {
    clear();
    return {};
  }
  if (now >= _rest_until) return {};
  return std::chrono::ceil<std::chrono::milliseconds>(_rest_until - now);
}

}  // namespace dannan::sched
