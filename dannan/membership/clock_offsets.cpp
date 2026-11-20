//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/membership/clock_offsets.h"

#include <algorithm>
#include <cstdlib>

namespace dannan::membership {

void ClockOffsets::record(const identity::NodeId& peer, uint64_t sent, uint64_t answered, uint64_t returned) {
  if (returned < sent) return;
  const auto rtt = returned - sent;
  // t1 - (t0 + t3) / 2, in signed arithmetic, without overflowing on Unix milliseconds.
  const auto offset = static_cast<int64_t>(answered) - static_cast<int64_t>(sent) - static_cast<int64_t>(rtt / 2);
  contract::Lock lock(_mutex);
  auto& samples = _samples[peer];
  samples.push_back({offset, rtt});
  while (samples.size() > window) samples.pop_front();
}

std::optional<std::chrono::milliseconds> ClockOffsets::offset(const identity::NodeId& peer) const {
  contract::Lock lock(_mutex);
  const auto found = _samples.find(peer);
  if (found == _samples.end() || found->second.empty()) return std::nullopt;
  const auto best = std::min_element(found->second.begin(), found->second.end(), [](const Sample& a, const Sample& b) { return a.rtt_ms < b.rtt_ms; });
  return std::chrono::milliseconds(best->offset_ms);
}

void ClockOffsets::forget(const identity::NodeId& peer) {
  contract::Lock lock(_mutex);
  _samples.erase(peer);
}

bool ClockOffsets::out_of_bounds(const identity::NodeId& self, const std::set<identity::NodeId>& roster, std::chrono::milliseconds bound) const {
  size_t past = 0;
  for (const auto& member : roster) {
    if (member == self) continue;
    const auto measured = offset(member);
    if (measured && std::llabs(measured->count()) > bound.count()) ++past;
  }
  return past > roster.size() / 2;
}

}  // namespace dannan::membership
