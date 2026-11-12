//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/memory_limit.h"

#include <algorithm>

namespace dannan::sched {
namespace {

// A GCC and clang extension, which -Wpedantic would otherwise refuse.
__extension__ typedef unsigned __int128 u128;

std::string mib(uint64_t bytes) { return std::to_string(bytes >> 20) + "M"; }

}  // namespace

std::optional<std::string> fit_to_limit(uint64_t limit, MemoryPlan& plan) {
  if (!limit) return std::nullopt;
  uint64_t total = 0;
  for (const auto& part : plan.parts) total += part.bytes;
  if (total <= limit) return std::nullopt;
  const auto shrink = [&](std::vector<MemoryPart>& parts) {
    std::string said;
    for (auto& part : parts) {
      const auto was = part.bytes;
      part.bytes = std::max<uint64_t>(1ull << 20, static_cast<uint64_t>(static_cast<u128>(part.bytes) * limit / total));
      said += (said.empty() ? "" : ", ") + part.name + " " + mib(was) + " to " + mib(part.bytes);
    }
    return said;
  };
  const auto parts = shrink(plan.parts);
  const auto reserves = shrink(plan.reserves);
  return "MEMORY OVER LIMIT: the configured parts total " + mib(total) + ", over runtime.memory_bytes " + mib(limit) +
         "; each shrinks by the same fraction: " + parts + " (in-flight reserves " + reserves + ")";
}

}  // namespace dannan::sched
