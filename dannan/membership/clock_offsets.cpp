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
  // The shortest round trip; of equal ones the newest, so a clock stepped since shows at the next
  // probe when the path's delay is steady.
  const Sample* best = nullptr;
  for (const auto& sample : found->second)
    if (!best || sample.rtt_ms <= best->rtt_ms) best = &sample;
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

net::Bytes encode_probe(uint64_t sent) {
  net::Writer writer;
  writer.u64(sent);
  return writer.take();
}

uint64_t decode_probe(std::span<const uint8_t> payload) {
  net::Reader reader(payload);
  const auto sent = reader.u64();
  reader.finish();
  return sent;
}

net::Bytes encode_answer(const ProbeAnswer& answer) {
  net::Writer writer;
  writer.u64(answer.sent);
  writer.u64(answer.answered);
  return writer.take();
}

ProbeAnswer decode_answer(std::span<const uint8_t> payload) {
  net::Reader reader(payload);
  ProbeAnswer out;
  out.sent = reader.u64();
  out.answered = reader.u64();
  reader.finish();
  return out;
}

}  // namespace dannan::membership
