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
#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

// A hybrid logical clock (TODO/2026-11-15-hlc-design.md, reviewed by Dagda, Lir and Mimir):
// Kulkarni and Demirbas's stamp, (physical, logical), that never goes backwards, across restarts
// too, stays within a bound of wall time, and orders causally related events. A stamp with its
// node id is a total order.
namespace dannan::contract {

// 48 bits of wall milliseconds and 16 of counter in one u64, so a stamp compares as an integer.
struct HlcStamp {
  uint64_t value = 0;
  static constexpr HlcStamp of(uint64_t unix_ms, uint16_t logical) noexcept { return HlcStamp{(unix_ms << 16) | logical}; }
  uint64_t unix_ms() const noexcept { return value >> 16; }
  uint16_t logical() const noexcept { return static_cast<uint16_t>(value); }
  auto operator<=>(const HlcStamp&) const = default;
};

// The one encoding every system writes: 8 bytes, big-endian (Mimir's review, rule 3).
std::array<uint8_t, 8> encode(HlcStamp stamp) noexcept;
HlcStamp decode(std::span<const uint8_t, 8> bytes) noexcept;

// What receive() did with a peer's stamp.
struct HlcReceipt {
  // This node's stamp for the receipt: after the received one when it was merged.
  HlcStamp local;
  // False: the stamp was further ahead of local wall time than max_ahead, and the clock was not
  // pulled forward to it. The refusal is the clock's, not the data's: the event that carried the
  // stamp is the hosted system's to apply by its stored stamp.
  bool merged = true;
  // How far the received stamp's physical part was from local wall time (positive: ahead).
  std::chrono::milliseconds offset{0};
};

// The largest offsets seen in one peer's stamps (Mimir's review: the skew an operator needs).
struct PeerSkew {
  std::chrono::milliseconds ahead{0};
  std::chrono::milliseconds behind{0};
};

struct HlcStats {
  // Stamps not merged, being too far ahead.
  uint64_t not_merged = 0;
  // Logical parts that overflowed, moving the physical part on.
  uint64_t overflows = 0;
  // Ceiling raises on the write path (a jump or a storm), not the roster loop's.
  uint64_t raises_off_loop = 0;
  // Per peer, from receive(stamp, peer).
  std::map<std::string, PeerSkew, std::less<>> peers;
};

// On the Context: one clock per node (named HlcClock: contract::Clock is the steady clock), shared by every hosted system. Thread-safe. now() and
// receive() hold no lock and wait on nothing but in one case: a stamp that would pass the
// persisted ceiling raises it first, one durable write (a receive far ahead, an overflow storm);
// the roster loop keeps the ceiling two seconds ahead so the steady state never meets it. A
// ceiling that cannot be raised throws, the stamp unissued.
class HlcClock {
 public:
  virtual ~HlcClock() = default;
  // A stamp for an event here: after every stamp this node has made or received.
  virtual HlcStamp now() = 0;
  // A peer's stamp, merged into this clock unless it is too far ahead.
  virtual HlcReceipt receive(HlcStamp remote) = 0;
  // The same, its offset kept per peer for Status (`peer` names it: a node id's text). Takes a
  // short lock for that table.
  virtual HlcReceipt receive(HlcStamp remote, std::string_view peer) = 0;
  // How far ahead of local wall time a received stamp may be (NodeConfig, default one minute).
  virtual std::chrono::milliseconds max_ahead() const = 0;
  virtual HlcStats stats() const = 0;
};

// Where the ceiling is kept: durable before raise() returns. The node's are its sealed counters
// (label "dannan/hlc"), or a durable file when it has no key custody.
class HlcCeiling {
 public:
  virtual ~HlcCeiling() = default;
  // Zero if never raised.
  virtual uint64_t load() = 0;
  // Throws if it cannot be made durable.
  virtual void raise(uint64_t unix_ms) = 0;
};

class HybridClock final : public HlcClock {
 public:
  // How far the roster loop keeps the ceiling ahead of what was issued.
  static constexpr std::chrono::milliseconds reserve{2000};

  // Begins after every stamp a run before could have issued: at the wall, or past the ceiling if
  // the wall is not, and raises the ceiling past it before returning (one durable write).
  HybridClock(const WallClock& wall, HlcCeiling& ceiling, std::chrono::milliseconds max_ahead = std::chrono::minutes(1));

  HlcStamp now() override;
  HlcReceipt receive(HlcStamp remote) override;
  HlcReceipt receive(HlcStamp remote, std::string_view peer) override;
  std::chrono::milliseconds max_ahead() const override { return _max_ahead; }
  HlcStats stats() const override;
  // The roster loop's, each second: the ceiling raised to what was issued (or the wall, if later)
  // plus the reserve, when it has fallen nearer than that. Waits on the state device.
  void keep_ahead();
  uint64_t ceiling() const noexcept { return _ceiling.load(); }

 private:
  // A stamp past `stamp`'s physical part may be issued: raised now if not.
  void allow(uint64_t unix_ms, bool on_loop);
  HlcReceipt merge(HlcStamp remote);

  const WallClock& _wall;
  HlcCeiling& _store;
  const std::chrono::milliseconds _max_ahead;
  std::atomic<uint64_t> _last{0};
  std::atomic<uint64_t> _ceiling{0};
  std::atomic<uint64_t> _not_merged{0};
  std::atomic<uint64_t> _overflows{0};
  std::atomic<uint64_t> _raises_off_loop{0};
  // One raise at a time; never held by now() or receive() but to raise.
  Mutex _raising;
  mutable Mutex _peers_mutex;
  std::map<std::string, PeerSkew, std::less<>> _peers DANNAN_GUARDED_BY(_peers_mutex);
};

}  // namespace dannan::contract
