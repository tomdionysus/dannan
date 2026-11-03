//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/node_id.h"

#include <algorithm>
#include <cstring>

#include "dannan/identity/crypto.h"

namespace dannan::identity {

NodeId NodeId::random() {
  NodeId id;
  const auto bytes = random_bytes(id.bytes.size());
  std::copy(bytes.begin(), bytes.end(), id.bytes.begin());
  return id;
}

std::optional<NodeId> NodeId::parse(const std::string& text) {
  if (text.size() != 32) return std::nullopt;
  const auto bytes = unhex(text);
  if (!bytes) return std::nullopt;
  NodeId id;
  std::copy(bytes->begin(), bytes->end(), id.bytes.begin());
  return id;
}

std::string to_string(const NodeId& id) { return hex(id.bytes); }

size_t NodeIdHash::operator()(const NodeId& id) const noexcept {
  size_t value = 0;
  std::memcpy(&value, id.bytes.data(), sizeof(value));
  return value;
}

}  // namespace dannan::identity
