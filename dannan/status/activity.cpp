//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/activity.h"

#include <algorithm>

namespace dannan::status {

int64_t ActivityClocks::now_ms() const { return std::chrono::duration_cast<std::chrono::milliseconds>(_time->now().time_since_epoch()).count(); }

void ActivityClocks::note(sched::FrameType frame, uint64_t bytes) {
  if (frame == sched::FrameType::foreground)
    _foreground.fetch_add(bytes, std::memory_order_relaxed);
  else if (frame == sched::FrameType::read_ahead)
    _read_ahead.fetch_add(bytes, std::memory_order_relaxed);
  else if (frame == sched::FrameType::loader)
    _loader.fetch_add(bytes, std::memory_order_relaxed);
  note(sched::work_class(frame));
}

void ActivityClocks::note(sched::WorkClass work) { _last[static_cast<size_t>(work)].store(now_ms() + 1, std::memory_order_relaxed); }

uint64_t ActivityClocks::take_bytes(sched::FrameType frame) {
  if (frame == sched::FrameType::foreground) return _foreground.exchange(0, std::memory_order_relaxed);
  if (frame == sched::FrameType::read_ahead) return _read_ahead.exchange(0, std::memory_order_relaxed);
  if (frame == sched::FrameType::loader) return _loader.exchange(0, std::memory_order_relaxed);
  return 0;
}

std::chrono::milliseconds ActivityClocks::idle_for(sched::WorkClass work) const {
  const auto last = _last[static_cast<size_t>(work)].load(std::memory_order_relaxed);
  if (!last) return std::chrono::hours(24);
  return std::chrono::milliseconds(std::max<int64_t>(0, now_ms() - (last - 1)));
}

}  // namespace dannan::status
