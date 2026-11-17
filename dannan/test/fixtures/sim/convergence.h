//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/durability.h"
#include "dannan/test/fixtures/sim/history.h"

// The convergence checker for the merge class (TODO/2026-11-15-simulation-harness.md, Mimir's
// review, ruling 4): merge writes are not linearizable by design. What must hold once the
// faults have healed and the cluster is quiet: every replica of a key holds the same value, and
// that value holds every write whose loss the faults do not excuse (durability.h's rules). The
// owner says what a value holds. Its search is compiled once, in dannan_sim_checkers.
namespace dannan::test::sim {

struct Replica {
  uint64_t node = 0;
  // None: the replica holds nothing for the key.
  std::optional<Bytes> value;
};

struct MergeWrite {
  // The owner's encoding of what was written.
  Bytes write;
  AckLevel level = AckLevel::replicated;
  // The node that acknowledged it, for ephemeral and persisted.
  uint64_t node = 0;
  uint64_t invoked_ms = 0;
  uint64_t answered_ms = 0;
};

// Whether `value` holds `write`: the owner's merge, read back.
using Holds = std::function<bool(const Bytes& value, const Bytes& write)>;

struct ConvergenceVerdict {
  bool ok = true;
  // Every replica holds the same value.
  bool converged = true;
  // Writes (by index) the value does not hold: lost, a defect; or excused by the faults.
  std::vector<size_t> lost;
  std::vector<size_t> excused;
  std::string why;
};

// Judges the replicas of one key, of a cluster of `nodes` acceptors, against the writes
// acknowledged to it and the faults the history records.
ConvergenceVerdict check_convergence(const std::vector<Replica>& replicas, const std::vector<MergeWrite>& writes, const std::vector<Fault>& faults,
                                     size_t nodes, const Holds& holds);

}  // namespace dannan::test::sim
