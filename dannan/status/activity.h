//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

#include "dannan/contract/time_source.h"
#include "dannan/sched/work.h"

namespace dannan::status {

// When work of each class was last present on this node, and the bytes each
// data frame class moved since the last take. Presence is per work class
// (sched::work_class), with bytes or without (a request): a foreground user
// is present whether it moved bytes or not, and the loader is kept apart so
// an import neither looks like a user nor leaves maintenance believing the
// node idle. Lock-free; never waits; safe from any thread.
class ActivityClocks {
 public:
  explicit ActivityClocks(const contract::TimeSource* time = &contract::steady_time_source()) : _time(time) {}

  void note(sched::FrameType frame, uint64_t bytes = 0);
  void note(sched::WorkClass work);
  // The bytes since the last take, for foreground, read-ahead and loader.
  uint64_t take_bytes(sched::FrameType frame);
  // 24 h when the class has never been present.
  std::chrono::milliseconds idle_for(sched::WorkClass work) const;
  bool recently_active(sched::WorkClass work, std::chrono::milliseconds window) const { return idle_for(work) < window; }

 private:
  int64_t now_ms() const;

  const contract::TimeSource* const _time;
  std::atomic<uint64_t> _foreground{0};
  std::atomic<uint64_t> _read_ahead{0};
  std::atomic<uint64_t> _loader{0};
  // Milliseconds since the steady clock's epoch, plus one, so zero is never.
  std::array<std::atomic<int64_t>, 4> _last{};
};

}  // namespace dannan::status
