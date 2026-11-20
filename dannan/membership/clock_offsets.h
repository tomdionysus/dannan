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
#include <deque>
#include <map>
#include <optional>
#include <set>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/node_id.h"

// Each peer's clock offset, from clock probes on the heartbeat (TODO/2026-11-20-stack-plan.md,
// item 6), and the rule that takes this node out of bounds. A probe is NTP's: sent at t0 on this
// node's wall, answered at t1 on the peer's, back at t3 on this node's; the offset is t1 less the
// midpoint of t0 and t3. Of a peer's recent probes the one with the shortest round trip is
// believed, as NTP's clock filter does: its offset is the least bent by an asymmetric path.
// Thread-safe.
namespace dannan::membership {

class ClockOffsets {
 public:
  // How many recent probes are kept per peer.
  static constexpr size_t window = 8;

  // A probe's three times, in Unix milliseconds. One whose round trip is negative (t3 before t0:
  // this node's wall stepped back between) is not kept.
  void record(const identity::NodeId& peer, uint64_t sent, uint64_t answered, uint64_t returned);
  // Positive: the peer's clock is ahead of this node's. Nothing before a probe has come back.
  std::optional<std::chrono::milliseconds> offset(const identity::NodeId& peer) const;
  void forget(const identity::NodeId& peer);

  // Whether this node's clock is out of bounds: its offset is past `bound` against more than half
  // of `roster`, itself counted at zero and a peer not yet probed counted as within. One or two
  // nodes can never reach that majority against themselves, so they are never out by this rule.
  bool out_of_bounds(const identity::NodeId& self, const std::set<identity::NodeId>& roster, std::chrono::milliseconds bound) const;

 private:
  struct Sample {
    int64_t offset_ms;
    uint64_t rtt_ms;
  };
  mutable contract::Mutex _mutex;
  std::map<identity::NodeId, std::deque<Sample>> _samples DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::membership
