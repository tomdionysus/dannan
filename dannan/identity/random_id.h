//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dannan/identity/crypto.h"

// A random 16-byte id (TODO/2026-11-20-stack-plan.md, item 16): a task, a request, a record, a
// transaction. One type per kind of thing, named by a tag, so a TaskId is never passed where a
// RequestId is wanted. Sixteen bytes from the system's CSPRNG: two never meet in practice.
namespace dannan::identity {

template <class Tag>
struct RandomId {
  std::array<uint8_t, 16> bytes{};
  auto operator<=>(const RandomId&) const = default;

  static RandomId random() {
    RandomId id;
    const auto drawn = random_bytes(id.bytes.size());
    std::copy(drawn.begin(), drawn.end(), id.bytes.begin());
    return id;
  }
  // Thirty-two hex digits, either case; nothing on anything else.
  static std::optional<RandomId> parse(std::string_view text) {
    if (text.size() != 32) return std::nullopt;
    const auto decoded = unhex(std::string(text));
    if (!decoded || decoded->size() != 16) return std::nullopt;
    RandomId id;
    std::copy(decoded->begin(), decoded->end(), id.bytes.begin());
    return id;
  }
};

// Thirty-two lowercase hex digits.
template <class Tag>
std::string to_string(const RandomId<Tag>& id) {
  return hex(id.bytes);
}

template <class Tag>
struct RandomIdHash {
  size_t operator()(const RandomId<Tag>& id) const noexcept {
    // Random bytes: the first word is as good a hash as any.
    size_t out;
    std::memcpy(&out, id.bytes.data(), sizeof(out));
    return out;
  }
};

}  // namespace dannan::identity
