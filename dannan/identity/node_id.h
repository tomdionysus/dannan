//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>

namespace dannan::identity {

// A node's identity on the wire: sixteen random bytes, created once and kept
// under the state path. Until protocol 25 it is self-chosen; from then it is
// the certificate's. Ordered, because a simultaneous dial is settled by
// which node id is lower.
struct NodeId {
  std::array<uint8_t, 16> bytes{};
  auto operator<=>(const NodeId&) const = default;

  static NodeId random();
  // Thirty-two hex digits, either case; empty on anything else.
  static std::optional<NodeId> parse(const std::string& text);
};

// Thirty-two lowercase hex digits.
std::string to_string(const NodeId&);

struct NodeIdHash {
  size_t operator()(const NodeId& id) const noexcept;
};

}  // namespace dannan::identity
