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

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dannan::sched {

// A background loop's share of time against the work it must not hold up
// (law 2 and law 3 before speculative work): a duty cycle by weight, not a
// bandwidth cap. While nothing contends, background work runs freely
// (work-conserving). While something does, a burst runs for at most one
// slice and then rests for its active time times foreground:background, so
// 95:5 gives the background a twentieth of the time. Both weights are at
// least one: background work is paced, never stopped.
//
// The loop asks may_start() before a burst, says started() (and serving()
// if setup came first), asks should_yield() between pieces of work, and
// says finished() at the end, which returns the rest it has earned. Several
// workers may share one DutyShare; the rest is charged when the last ends.
// `contended` is the caller's reading, at that moment, of whether the work
// it yields to is present. Thread-safe; waits on nothing.
class DutyShare {
 public:
  // Throws std::invalid_argument for a zero weight or slice.
  DutyShare(uint32_t foreground_weight, uint32_t background_weight, std::chrono::milliseconds slice = std::chrono::milliseconds(25));

  bool may_start(contract::TimePoint now, bool contended);
  // `serving` false: the worker is setting up, counted as active but not
  // charged until serving().
  void started(contract::TimePoint now, bool contended, bool serving = true);
  void serving(contract::TimePoint now, bool contended);
  // Contention that arrives mid-burst ends the burst at once.
  bool should_yield(contract::TimePoint now, bool contended);
  // The rest earned: zero uncontended, or while other workers still run.
  std::chrono::milliseconds finished(contract::TimePoint now, bool contended);
  // How long until may_start() can say yes; zero now.
  std::chrono::milliseconds wait(contract::TimePoint now, bool contended);

 private:
  void clear() DANNAN_REQUIRES(_mutex);
  void begin(contract::TimePoint now) DANNAN_REQUIRES(_mutex);

  const uint32_t _foreground_weight;
  const uint32_t _background_weight;
  const std::chrono::milliseconds _slice;
  contract::Mutex _mutex;
  std::optional<contract::TimePoint> _burst DANNAN_GUARDED_BY(_mutex);
  contract::TimePoint _slice_ends DANNAN_GUARDED_BY(_mutex){};
  contract::TimePoint _rest_until DANNAN_GUARDED_BY(_mutex){};
  size_t _workers DANNAN_GUARDED_BY(_mutex) = 0;
};

}  // namespace dannan::sched
