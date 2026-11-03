//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <string>

#include "dannan/identity/node_id.h"
#include "dannan/net/codec.h"

namespace dannan::membership {

// Self-declared node properties, gossiped so every peer makes the same
// dialling and placement decisions; until protocol 25 they are the
// capability set, read from this byte. Both set is an ordinary dialable
// storage node.
inline constexpr uint8_t flag_inbound_capable = 1U << 0;
inline constexpr uint8_t flag_hosts_extents = 1U << 1;
inline constexpr uint8_t flags_default = flag_inbound_capable | flag_hosts_extents;

constexpr uint8_t flags_for(bool inbound_capable, bool hosts_extents) noexcept {
  return static_cast<uint8_t>((inbound_capable ? flag_inbound_capable : 0U) | (hosts_extents ? flag_hosts_extents : 0U));
}

// A node as the roster gossips it and every handshake carries it: protocol
// 23's record, grandfathered. capacity, used, metadata_generation and the
// write floor are the store's figures; Dannan carries them without reading
// them until protocol 25 replaces them with a generic advertisement map.
struct NodeRecord {
  identity::NodeId id;
  std::string host;
  std::string failure_domain;
  uint16_t port = 0;
  uint64_t capacity = 0;
  uint64_t used = 0;
  uint64_t seen_unix_ms = 0;
  uint64_t metadata_generation = 0;
  // Every node advertising a write floor must advertise the same one; a
  // mismatch fails closed.
  uint32_t metadata_write_replicas_required = 0;
  uint8_t flags = flags_default;

  // Peers never dial a node without it: it reaches them, and they answer
  // over the sessions it opened.
  bool inbound_capable() const noexcept { return (flags & flag_inbound_capable) != 0; }
  // A node without it is never a DATA placement candidate.
  bool hosts_extents() const noexcept { return (flags & flag_hosts_extents) != 0; }

  bool operator==(const NodeRecord&) const = default;
};

void encode(net::Writer&, const NodeRecord&);
// Strings are bounded at 4096 bytes. Throws net::DecodeError.
NodeRecord decode(net::Reader&);

}  // namespace dannan::membership
