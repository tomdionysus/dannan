//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dannan::sched {

// The memory a node is configured to use, by part, named as the operator
// names it in configuration (Dannan's retained total, Dagda's caches, a
// hosted system's own), and the in-flight reserves inside the retained total.
struct MemoryPart {
  std::string name;
  uint64_t bytes = 0;
};

struct MemoryPlan {
  std::vector<MemoryPart> parts;
  std::vector<MemoryPart> reserves;
};

// Fits the plan under `limit` bytes (none when zero). Over it, every part and
// every reserve shrinks by the same fraction, limit / total, never below
// 1 MiB, and the node starts. Returns the one warning to log when anything
// shrank ("MEMORY OVER LIMIT: ..."), naming each part's old and new size.
std::optional<std::string> fit_to_limit(uint64_t limit, MemoryPlan& plan);

}  // namespace dannan::sched
