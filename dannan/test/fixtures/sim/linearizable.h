//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/history.h"

// The linearizability checker (TODO/2026-11-15-simulation-harness.md, Mimir's): a
// Wing-Gong search, with Lowe's memo of (operations taken, state), over each group of keys
// that operations touch together, against a model the owner supplies. Its search is
// compiled once, in dannan_sim_checkers; a model is the owner's, in its own tests.
namespace dannan::test::sim {

// An owner's sequential specification over one group of keys, its states as bytes (the
// owner's encoding; equal bytes are equal states).
class Model {
 public:
  virtual ~Model() = default;
  // The state of `keys` (sorted) before any operation.
  virtual std::string initial(const std::vector<std::string>& keys) const = 0;
  // The states `state` may move to by `operation` with `result` (Mimir's review: allowed
  // results, not one answer). Empty: not allowed. `result` null: the result is unknown (the
  // operation was never answered), and every state it could lead to is allowed.
  virtual std::vector<std::string> step(const std::string& state, const Operation& operation, const Bytes* result) const = 0;
};

struct Verdict {
  // Every group explains its history; false if one does not, or the search ran out.
  bool linearizable = true;
  // The search ran out of `budget` before deciding a group: no verdict for it.
  bool exhausted = false;
  // The first group that failed: its keys, and the operations (by index) that no order
  // explains, as far as the search got.
  std::vector<std::string> keys;
  std::vector<size_t> unexplained;
  std::string why;
  // Search steps taken, over every group.
  size_t steps = 0;
};

// Whether some order of the history's operations, each taking effect at one instant between
// its invocation and its answer, explains every result under `model`. An operation never
// answered (or answered provisionally and not yet resolved) may take effect at any time after
// its invocation, or never; one cancelled, or resolved as not taken, never does. `budget`
// bounds the search steps over all groups.
Verdict check_linearizable(const History& history, const Model& model, size_t budget = 2'000'000);

}  // namespace dannan::test::sim
