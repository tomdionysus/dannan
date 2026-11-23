//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/contract/hlc.h"

#include <algorithm>

namespace dannan::contract {
namespace {

constexpr uint32_t logical_limit = 0xFFFF;

// (physical, logical) with a logical part that may have overflowed: moved on a millisecond,
// never wrapped.
struct Next {
  uint64_t physical;
  uint32_t logical;
  bool overflowed = false;
};

Next settled(uint64_t physical, uint32_t logical) {
  if (logical > logical_limit) return Next{physical + 1, 0, true};
  return Next{physical, logical};
}

}  // namespace

std::array<uint8_t, 8> encode(HlcStamp stamp) noexcept {
  std::array<uint8_t, 8> out{};
  for (size_t i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(stamp.value >> (8 * (7 - i)));
  return out;
}

HlcStamp decode(std::span<const uint8_t, 8> bytes) noexcept {
  uint64_t value = 0;
  for (const auto byte : bytes) value = (value << 8) | byte;
  return HlcStamp{value};
}

HybridClock::HybridClock(const WallClock& wall, HlcCeiling& ceiling, std::chrono::milliseconds max_ahead)
    : _wall(wall), _store(ceiling), _max_ahead(max_ahead) {
  // Every stamp a run before issued had a physical part at most the ceiling it persisted, so
  // beginning past the ceiling, or at a wall already past it, is after them all.
  const auto persisted = ceiling.load();
  const auto start = persisted >= wall.unix_ms() ? persisted + 1 : wall.unix_ms();
  _last.store(HlcStamp::of(start, 0).value);
  // Nothing written: the first stamp is past `start`, and allow() raises the ceiling past it first.
  _ceiling.store(persisted);
}

void HybridClock::allow(uint64_t unix_ms, bool on_loop) {
  if (unix_ms <= _ceiling.load()) return;
  Lock lock(_raising);
  if (unix_ms <= _ceiling.load()) return;
  const auto raised = unix_ms + static_cast<uint64_t>(reserve.count());
  _store.raise(raised);
  _ceiling.store(raised);
  if (!on_loop) ++_raises_off_loop;
}

HlcStamp HybridClock::now() {
  if (_out_of_bounds.load()) throw ClockOutOfBounds("this node's clock is out of bounds against its peers: it issues no stamp until it is back");
  for (;;) {
    auto previous = _last.load();
    const HlcStamp last{previous};
    const auto wall = _wall.unix_ms();
    const auto next = wall > last.unix_ms() ? Next{wall, 0} : settled(last.unix_ms(), last.logical() + 1u);
    allow(next.physical, false);
    const auto stamp = HlcStamp::of(next.physical, static_cast<uint16_t>(next.logical));
    if (_last.compare_exchange_weak(previous, stamp.value)) {
      if (next.overflowed) ++_overflows;
      _issued.store(true);
      return stamp;
    }
  }
}

HlcReceipt HybridClock::merge(HlcStamp remote) {
  const auto wall = _wall.unix_ms();
  const auto offset = std::chrono::milliseconds(static_cast<int64_t>(remote.unix_ms()) - static_cast<int64_t>(wall));
  if (remote.unix_ms() > wall + static_cast<uint64_t>(_max_ahead.count())) {
    ++_not_merged;
    // Out of bounds, this node issues nothing, so the receipt carries the last stamp it made.
    return HlcReceipt{_out_of_bounds.load() ? HlcStamp{_last.load()} : now(), false, offset};
  }
  for (;;) {
    auto previous = _last.load();
    const HlcStamp last{previous};
    const auto physical = std::max({last.unix_ms(), remote.unix_ms(), wall});
    uint32_t logical = 0;
    if (physical == last.unix_ms() && physical == remote.unix_ms())
      logical = std::max(last.logical(), remote.logical()) + 1u;
    else if (physical == last.unix_ms())
      logical = last.logical() + 1u;
    else if (physical == remote.unix_ms())
      logical = remote.logical() + 1u;
    const auto next = settled(physical, logical);
    allow(next.physical, false);
    const auto stamp = HlcStamp::of(next.physical, static_cast<uint16_t>(next.logical));
    if (_last.compare_exchange_weak(previous, stamp.value)) {
      if (next.overflowed) ++_overflows;
      _issued.store(true);
      return HlcReceipt{stamp, true, offset};
    }
  }
}

HlcReceipt HybridClock::receive(HlcStamp remote) { return merge(remote); }

HlcReceipt HybridClock::receive(HlcStamp remote, std::string_view peer) {
  const auto receipt = merge(remote);
  Lock lock(_peers_mutex);
  auto found = _peers.find(peer);
  if (found == _peers.end()) found = _peers.emplace(std::string(peer), PeerSkew{}).first;
  if (receipt.offset.count() > 0) found->second.ahead = std::max(found->second.ahead, receipt.offset);
  if (receipt.offset.count() < 0) found->second.behind = std::max(found->second.behind, -receipt.offset);
  return receipt;
}

const char* admit_verdict_name(AdmitVerdict verdict) noexcept {
  switch (verdict) {
    case AdmitVerdict::admitted:
      return "admitted";
    case AdmitVerdict::ahead:
      return "ahead";
    case AdmitVerdict::self_out_of_bounds:
      return "self_out_of_bounds";
  }
  return "unknown";
}

Admission HybridClock::admit(HlcStamp remote, std::string_view peer) {
  if (_out_of_bounds.load()) {
    const auto wall = _wall.unix_ms();
    const auto offset = std::chrono::milliseconds(static_cast<int64_t>(remote.unix_ms()) - static_cast<int64_t>(wall));
    return Admission{HlcReceipt{HlcStamp{_last.load()}, false, offset}, AdmitVerdict::self_out_of_bounds,
                     "this node's clock is out of bounds against its peers, so it judges no stamp now"};
  }
  const auto receipt = receive(remote, peer);
  if (receipt.merged) return Admission{receipt, AdmitVerdict::admitted, {}};
  return Admission{receipt, AdmitVerdict::ahead,
                   "the clock of " + std::string(peer) + " is " + std::to_string(receipt.offset.count()) + " ms ahead of this node's, past the " +
                       std::to_string(_max_ahead.count()) + " ms allowed"};
}

HlcStats HybridClock::stats() const {
  HlcStats out;
  out.not_merged = _not_merged.load();
  out.overflows = _overflows.load();
  out.raises_off_loop = _raises_off_loop.load();
  Lock lock(_peers_mutex);
  out.peers = _peers;
  return out;
}

void HybridClock::keep_ahead() {
  if (!_issued.load()) return;
  const auto issued = HlcStamp{_last.load()}.unix_ms();
  if (_ceiling.load() >= issued + static_cast<uint64_t>(reserve.count())) return;
  Lock lock(_raising);
  const auto raised = issued + static_cast<uint64_t>(reserve.count());
  if (_ceiling.load() >= raised) return;
  _store.raise(raised);
  _ceiling.store(raised);
}

}  // namespace dannan::contract
