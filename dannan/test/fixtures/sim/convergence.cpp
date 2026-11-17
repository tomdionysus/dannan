//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/test/fixtures/sim/convergence.h"

namespace dannan::test::sim {

ConvergenceVerdict check_convergence(const std::vector<Replica>& replicas, const std::vector<MergeWrite>& writes, const std::vector<Fault>& faults,
                                     size_t nodes, const Holds& holds) {
  ConvergenceVerdict verdict;
  if (replicas.empty()) {
    verdict.ok = false;
    verdict.why = "no replica to judge";
    return verdict;
  }
  for (const auto& replica : replicas)
    if (replica.value != replicas.front().value) {
      verdict.ok = verdict.converged = false;
      verdict.why = "node " + std::to_string(replica.node) + "'s replica differs from node " + std::to_string(replicas.front().node) + "'s";
      break;
    }
  // Judged on the first replica's value: if they differ, that is the defect already named, and
  // a write missing from the first is a loss besides.
  const auto& value = replicas.front().value;
  for (size_t i = 0; i < writes.size(); ++i) {
    const auto& write = writes[i];
    if (value && holds(*value, write.write)) continue;
    if (loss_excused(faults, write.level, write.invoked_ms, write.answered_ms, write.node, nodes)) {
      verdict.excused.push_back(i);
      continue;
    }
    verdict.lost.push_back(i);
    if (verdict.ok) {
      verdict.ok = false;
      verdict.why = "write " + std::to_string(i) + ", acknowledged at " + std::to_string(write.answered_ms) + " ms, is not held, and no fault excuses its loss";
    }
  }
  return verdict;
}

}  // namespace dannan::test::sim
