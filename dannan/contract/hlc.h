//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// The hybrid logical clock every hosted system on a node shares.

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dannan::contract {

/// A hybrid logical clock stamp (TODO/2026-11-15-hlc-design.md, reviewed by Dagda, Lir and Mimir):
/// Kulkarni and Demirbas's (physical, logical) pair, 48 bits of wall milliseconds and 16 of counter
/// in one u64, so a stamp compares as an integer.
///
/// Why: it never goes backwards (across restarts too), stays within a bound of wall time, and
/// orders causally related events, so a last-writer-wins register or a commit order can trust it.
/// A stamp with its node's id is a total order.
struct HlcStamp {
  /// The packed stamp: physical milliseconds above, logical counter in the low 16 bits.
  uint64_t value = 0;
  /// The stamp for `unix_ms` and `logical`.
  static constexpr HlcStamp of(uint64_t unix_ms, uint16_t logical) noexcept { return HlcStamp{(unix_ms << 16) | logical}; }
  /// Its physical part, Unix milliseconds.
  uint64_t unix_ms() const noexcept { return value >> 16; }
  /// Its logical part.
  uint16_t logical() const noexcept { return static_cast<uint16_t>(value); }
  /// Stamps order as their values.
  auto operator<=>(const HlcStamp&) const = default;
};

/// The one encoding every system writes: 8 bytes, big-endian (Mimir's review: one form, stack-wide).
std::array<uint8_t, 8> encode(HlcStamp stamp) noexcept;
/// The stamp 8 big-endian bytes hold.
HlcStamp decode(std::span<const uint8_t, 8> bytes) noexcept;

/// What HlcClock::receive() did with a peer's stamp.
struct HlcReceipt {
  /// This node's stamp for the receipt: after the received one when it was merged.
  HlcStamp local;
  /// False: the stamp was further ahead of local wall time than max_ahead, and the clock was not
  /// pulled forward to it. The refusal is the clock's, not the data's.
  bool merged = true;
  /// How far the received stamp's physical part was from local wall time (positive: ahead).
  std::chrono::milliseconds offset{0};
};

/// The largest offsets seen in one peer's stamps: the skew an operator needs (Mimir's review).
struct PeerSkew {
  /// The furthest ahead of local wall time.
  std::chrono::milliseconds ahead{0};
  /// The furthest behind.
  std::chrono::milliseconds behind{0};
};

/// What HlcClock::admit() decided about a peer's stamp (the clock rules, item 6).
enum class AdmitVerdict : uint8_t {
  /// Within max_ahead of this node's wall: merged, and what it carries may be applied.
  admitted,
  /// Past max_ahead: refused, and never applied by its stamp.
  ahead,
  /// This node's own clock is out of bounds against its peers, so it judges no stamp: refused, and
  /// worth trying again once it is back.
  self_out_of_bounds,
};
/// "admitted", "ahead" or "self_out_of_bounds".
const char* admit_verdict_name(AdmitVerdict verdict) noexcept;

/// HlcClock::admit()'s answer.
struct Admission {
  /// What receive() did.
  HlcReceipt receipt;
  /// The verdict.
  AdmitVerdict verdict = AdmitVerdict::admitted;
  /// Empty when admitted; otherwise for a person, naming the peer and how far.
  std::string reason;
  /// Whether the stamp was admitted.
  bool admitted() const noexcept { return verdict == AdmitVerdict::admitted; }
};

/// What HlcClock::now() throws while this node's clock is out of bounds against its peers: it
/// issues no stamp until it is back, and never waits for that.
class ClockOutOfBounds : public std::runtime_error {
 public:
  /// With a message for a person.
  using std::runtime_error::runtime_error;
};

/// What a clock has done, for Status.
struct HlcStats {
  /// Stamps not merged, being too far ahead.
  uint64_t not_merged = 0;
  /// Logical parts that overflowed, moving the physical part on.
  uint64_t overflows = 0;
  /// Ceiling raises on the write path (the first stamp after an idle spell, a jump, a storm), not
  /// the roster loop's.
  uint64_t raises_off_loop = 0;
  /// Per peer, from receive(stamp, peer) and admit().
  std::map<std::string, PeerSkew, std::less<>> peers;
};

/// The node's hybrid logical clock, as Context::hlc(): one per node, shared by every hosted system
/// (named HlcClock because contract::Clock is the steady clock).
///
/// The rule a receiver follows: admit() every peer's stamp before applying what it carries (Dagda
/// once per commit, on the commit's highest stamp; an agreement acceptor; Lir). A stamp past
/// max_ahead is refused, so a node with a runaway clock cannot drag the cluster's stamps forward.
///
/// Durability: a stamp is never issued past the persisted ceiling, so after a restart the clock
/// begins after every stamp the run before issued. now() and receive() hold no lock and wait on
/// nothing but in one case: a stamp that would pass the ceiling raises it first, one durable
/// write. The roster loop keeps the ceiling ahead while stamps are issued, so the steady state
/// never meets it; an idle node writes nothing (0.12.0). A ceiling that cannot be raised throws,
/// the stamp unissued. Thread-safe.
///
/// @code
/// auto& hlc = context.hlc();
/// const auto stamp = hlc.now();                     // for a write here
/// const auto admitted = hlc.admit(their_stamp, peer_id_text);
/// if (!admitted.admitted()) return refuse(admitted.reason);  // retry later for self_out_of_bounds
/// @endcode
class HlcClock {
 public:
  virtual ~HlcClock() = default;
  /// A stamp for an event here: after every stamp this node has made or received.
  /// @throws ClockOutOfBounds at once while this node's clock is out of bounds (issuing() false).
  /// @throws std::runtime_error when the ceiling it needs cannot be raised.
  virtual HlcStamp now() = 0;
  /// Merges a peer's stamp into this clock, unless it is more than max_ahead() ahead.
  virtual HlcReceipt receive(HlcStamp remote) = 0;
  /// The same, its offset kept per peer for Status. Takes a short lock for that table.
  /// @param peer names the peer: its node id's text.
  virtual HlcReceipt receive(HlcStamp remote, std::string_view peer) = 0;
  /// How far ahead of local wall time a received stamp may be (NodeConfig::hlc_max_ahead, five
  /// seconds by default).
  virtual std::chrono::milliseconds max_ahead() const = 0;
  /// The one check every receiver makes of a peer's stamp before applying what it carries: merged
  /// when admitted; refused, and so never applied by its stamp, when it is past max_ahead() or when
  /// this node is out of bounds. Feeds the per-peer skew. Never throws.
  virtual Admission admit(HlcStamp remote, std::string_view peer) = 0;
  /// Whether this node issues stamps: false while its clock offset is past the bound against more
  /// than half of the agreed roster (the node decides, from its clock probes).
  virtual bool issuing() const = 0;
  /// The node's call, from its clock probes.
  virtual void set_out_of_bounds(bool out) = 0;
  /// What the clock has done.
  virtual HlcStats stats() const = 0;
};

/// Where a clock's ceiling is kept: durable before raise() returns. The node's are its sealed
/// counters (label "dannan/hlc"), or a durable file when it has no key custody.
class HlcCeiling {
 public:
  virtual ~HlcCeiling() = default;
  /// The ceiling persisted, Unix milliseconds; zero if never raised.
  virtual uint64_t load() = 0;
  /// Persists a higher ceiling.
  /// @throws std::runtime_error if it cannot be made durable.
  virtual void raise(uint64_t unix_ms) = 0;
};

/// Dannan's HlcClock, over a WallClock and an HlcCeiling.
class HybridClock final : public HlcClock {
 public:
  /// How far ahead of the last stamp issued the roster loop keeps the ceiling: inside max_ahead's
  /// five seconds, so a crash restart's stamps are never refused as ahead.
  static constexpr std::chrono::milliseconds reserve{2000};

  /// Begins after every stamp a run before could have issued: at the wall, or past the ceiling if
  /// the wall is not. Writes nothing (0.12.0): the first stamp raises the ceiling past itself
  /// before it is issued, so a node that never stamps never touches the file.
  /// @param wall the wall clock; it must outlive the clock.
  /// @param ceiling where the ceiling persists; it must outlive the clock.
  /// @param max_ahead how far ahead a received stamp may be.
  HybridClock(const WallClock& wall, HlcCeiling& ceiling, std::chrono::milliseconds max_ahead = std::chrono::seconds(5));

  /// As HlcClock::now().
  HlcStamp now() override;
  /// As HlcClock::receive().
  HlcReceipt receive(HlcStamp remote) override;
  /// As HlcClock::receive(), kept per peer.
  HlcReceipt receive(HlcStamp remote, std::string_view peer) override;
  /// As HlcClock::max_ahead().
  std::chrono::milliseconds max_ahead() const override { return _max_ahead; }
  /// As HlcClock::admit().
  Admission admit(HlcStamp remote, std::string_view peer) override;
  /// As HlcClock::issuing().
  bool issuing() const override { return !_out_of_bounds.load(); }
  /// As HlcClock::set_out_of_bounds().
  void set_out_of_bounds(bool out) override { _out_of_bounds.store(out); }
  /// As HlcClock::stats().
  HlcStats stats() const override;
  /// The roster loop's, each second: the ceiling raised to the last stamp issued plus the reserve,
  /// when it has fallen nearer than that. It follows the stamps, never the wall, so an idle node
  /// writes nothing (0.12.0: it once followed the wall and wrote every second, Lir's finding).
  /// Waits on the state device when it writes.
  void keep_ahead();
  /// The ceiling now, Unix milliseconds.
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
  std::atomic<bool> _out_of_bounds{false};
  // Whether this run has issued a stamp (now, or a merge): until then keep_ahead has nothing to
  // keep ahead of.
  std::atomic<bool> _issued{false};
  // One raise at a time; never held by now() or receive() but to raise.
  Mutex _raising;
  mutable Mutex _peers_mutex;
  std::map<std::string, PeerSkew, std::less<>> _peers DANNAN_GUARDED_BY(_peers_mutex);
};

}  // namespace dannan::contract
