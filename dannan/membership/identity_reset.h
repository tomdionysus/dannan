//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/identity/node_id.h"
#include "dannan/net/codec.h"

namespace dannan::membership {

// An operator's statement that an endpoint now belongs to a new node: the
// stale association is dropped everywhere, and only a direct connection or
// gossip newer than the reset re-establishes a node there. A freshness
// boundary, not a blacklist. The newest epoch for an endpoint wins.
struct IdentityReset {
  std::string host;
  // Zero: every port on the host.
  uint16_t port = 0;
  // Zero: every node at the endpoint.
  identity::NodeId stale_node_id;
  uint64_t epoch = 0;
  uint64_t reset_unix_ms = 0;
  identity::NodeId reset_by;
  std::string reason;

  bool matches_endpoint(std::string_view endpoint_host, uint16_t endpoint_port) const noexcept;
  bool matches_node(const identity::NodeId& node) const noexcept;
  // "[host]:port", or "[host]:*" for every port: one reset per key.
  std::string key() const;

  auto operator<=>(const IdentityReset&) const = default;
};

// Protocol 23's gossip payload (MACHIDR1), the identity_resets request and
// its reply. Decoding throws net::DecodeError for a bad magic, more than
// 65536 records, or a record with no host or epoch.
net::Bytes encode_identity_resets(const std::vector<IdentityReset>& resets);
std::vector<IdentityReset> decode_identity_resets(std::span<const uint8_t> payload);

}  // namespace dannan::membership
