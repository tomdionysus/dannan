//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Scope: may this principal write under this prefix (Tom, 2026-11-04: the
// seam is Dannan's, Lugh installs the source, every hosted system checks
// through it; TODO/2026-11-04-identity.md, section 6). Dannan declares the
// question; the answer is Lugh's, read from its scope head, so Dannan links
// neither Lugh nor Dagda.
namespace dannan::contract {

// Who is writing: a node of the cluster (its node id, 32 hex digits), or a
// principal the directory knows (Lugh's id for a user or a service).
struct Principal {
  enum class Kind : uint8_t { node, directory };
  Kind kind = Kind::directory;
  std::string id;
};

// The answer, with its evidence: the generation of the scope data it was
// read from (zero when no data was consulted), and why not when not.
struct ScopeDecision {
  bool allowed = false;
  uint64_t generation = 0;
  std::string why;
};

// Installed by the system that holds the scope data (Lugh). Answered from
// memory: never waits, never calls a peer. Thread-safe.
class ScopeSource {
 public:
  virtual ~ScopeSource() = default;
  // May `principal` write under `prefix` (a head space or object namespace
  // prefix, as the hosted system names it)?
  virtual ScopeDecision may_write(const Principal& principal, std::string_view prefix) const = 0;
};

}  // namespace dannan::contract
