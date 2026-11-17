//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "dannan/test/fixtures/sim/history.h"

// Durability judged against the faults (the harness design, Kvasir's review): each write was
// acknowledged at one of the stack's levels (the maintainer, 2026-11-15: ephemeral, persisted, decided,
// replicated), and losing it is a defect unless the faults recorded in the history excuse it.
// The events read: "kill" (the nodes stopped together, keeping only what each had synced),
// "wipe" (the nodes' disks lost), and "sync" (a node made its state durable; an event, not a
// fault).
namespace dannan::test::sim {

enum class AckLevel : uint8_t {
  // Held by its node in memory: lost with the node.
  ephemeral,
  // On its node's disk: lost only with that disk.
  persisted,
  // Chosen by a majority, each holding it in memory and syncing later: lost only if a majority
  // stopped together, none of them having synced since.
  decided,
  // On a majority's disks: lost only with a majority's disks.
  replicated,
};

// Whether losing a write acknowledged at `level` (on `node`, for ephemeral and persisted) is
// excused by the faults, in a cluster of `nodes` acceptors. It was invoked at `invoked_ms` and
// answered at `answered_ms`, and was taken somewhere between: a fault counts from the first, the
// earliest it could have been taken, and a sync only from the second, when it surely had been, so
// whatever the faults could excuse is excused.
inline bool loss_excused(const std::vector<Fault>& faults, AckLevel level, uint64_t invoked_ms, uint64_t answered_ms, uint64_t node, size_t nodes) {
  const size_t majority = nodes / 2 + 1;
  const auto after = [invoked_ms](const Fault& fault) { return fault.at_ms >= invoked_ms; };
  const auto names = [](const Fault& fault, uint64_t of) { return std::find(fault.nodes.begin(), fault.nodes.end(), of) != fault.nodes.end(); };
  // Distinct nodes whose disks were lost since the write: what excuses a replicated loss.
  std::set<uint64_t> wiped;
  for (const auto& fault : faults)
    if (after(fault) && fault.kind == "wipe") wiped.insert(fault.nodes.begin(), fault.nodes.end());
  switch (level) {
    case AckLevel::ephemeral:
      return std::any_of(faults.begin(), faults.end(),
                         [&](const Fault& fault) { return after(fault) && (fault.kind == "kill" || fault.kind == "wipe") && names(fault, node); });
    case AckLevel::persisted:
      return wiped.contains(node);
    case AckLevel::decided:
      if (wiped.size() >= majority) return true;
      // One kill of nodes together, enough of which had not synced since the write.
      for (const auto& kill : faults) {
        if (!after(kill) || kill.kind != "kill") continue;
        size_t unsynced = 0;
        for (const auto killed : kill.nodes) {
          const bool synced = std::any_of(faults.begin(), faults.end(), [&](const Fault& sync) {
            return sync.kind == "sync" && sync.at_ms >= answered_ms && sync.at_ms <= kill.at_ms && names(sync, killed);
          });
          // A wiped node's sync is gone with its disk.
          if (!synced || wiped.contains(killed)) ++unsynced;
        }
        if (unsynced >= majority) return true;
      }
      return false;
    case AckLevel::replicated:
      return wiped.size() >= majority;
  }
  return false;
}

}  // namespace dannan::test::sim
