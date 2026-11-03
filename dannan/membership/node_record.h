//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dannan/identity/node_id.h"
#include "dannan/net/codec.h"

namespace dannan::membership {

// What a node does for the cluster, gossiped so every peer makes the same
// dialling decisions. Unknown bits are kept as they came.
inline constexpr uint32_t capability_inbound = 1U << 0;  // dialable at host:port
inline constexpr uint32_t capability_relay = 1U << 1;    // forwards circuits for others
inline constexpr uint32_t capabilities_default = capability_inbound;

// What a node knows of its own reachability (wire v1, section 5): public is
// dial-back confirmed; punchable kept the same external port for two
// different peers, so a brokered simultaneous open can work; relay_only is
// neither.
enum class PathClass : uint8_t { public_path = 0, punchable = 1, relay_only = 2 };
const char* path_class_name(PathClass) noexcept;

// An address this node was seen at from outside: every handshake's
// observed address is one. Confirmed once a peer reached the node there.
struct Candidate {
  std::string host;
  uint16_t port = 0;
  bool confirmed = false;
  uint64_t confirmed_unix_ms = 0;
  bool operator==(const Candidate&) const = default;
};

// Bounds every decoder checks and every setter refuses past.
inline constexpr size_t max_candidates = 16;
inline constexpr size_t max_adverts = 64;
inline constexpr size_t max_advert_key = 256;
inline constexpr size_t max_advert_value = 1024;

// A node as the roster gossips it and every handshake carries it (wire v1,
// section 3). The adverts are the hosted system's: a store puts its
// capacity, use and write floor there, and Dannan carries them without
// reading them, except where a caller names one (placement's capacity).
struct NodeRecord {
  identity::NodeId id;
  std::string host;
  uint16_t port = 0;
  std::string failure_domain;
  uint64_t seen_unix_ms = 0;
  uint32_t capabilities = capabilities_default;
  PathClass path_class = PathClass::public_path;
  std::vector<Candidate> candidates;
  std::map<std::string, net::Bytes> adverts;

  // Peers never dial a node without it: it reaches them, and they answer
  // over the sessions it opened.
  bool inbound_capable() const noexcept { return (capabilities & capability_inbound) != 0; }
  bool relays() const noexcept { return (capabilities & capability_relay) != 0; }

  // The advert under `key`, or nothing.
  std::optional<std::span<const uint8_t>> advert(const std::string& key) const;
  // An advert holding a u64 (big-endian, eight bytes); nothing when absent
  // or of another width.
  std::optional<uint64_t> advert_u64(const std::string& key) const;

  bool operator==(const NodeRecord&) const = default;
};

// The eight bytes advert_u64 reads.
net::Bytes u64_advert(uint64_t value);

void encode(net::Writer&, const NodeRecord&);
// Strings are bounded at 4096 bytes, candidates, adverts and their sizes as
// above. Throws net::DecodeError, also for a path class out of range.
NodeRecord decode(net::Reader&);

}  // namespace dannan::membership
