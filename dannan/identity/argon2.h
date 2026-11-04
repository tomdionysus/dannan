//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "dannan/identity/secure_memory.h"

// Argon2id (RFC 9106), for the whole stack: Dannan stretches a typed
// passphrase into a node's custody root with it, Lugh hashes passwords with
// it. One home (Tom, 2026-11-03).
namespace dannan::identity {

// What one evaluation costs: memory in KiB, passes over it, and lanes.
// The default is RFC 9106's second recommended option (64 MiB, 3 passes,
// 4 lanes).
struct Argon2Costs {
  uint32_t memory_kib = 64 * 1024;
  uint32_t iterations = 3;
  uint32_t lanes = 4;

  // The most any evaluation may spend: costs read from a file or a record
  // are checked against it before anything is spent, so a crafted one
  // cannot make a node allocate a terabyte.
  static constexpr uint32_t max_memory_kib = 2u * 1024 * 1024;  // 2 GiB
  static constexpr uint32_t max_iterations = 64;
  static constexpr uint32_t max_lanes = 64;
  // At least one lane and one pass, 8 KiB per lane, and within the ceiling.
  bool valid() const noexcept {
    return lanes && lanes <= max_lanes && iterations && iterations <= max_iterations && memory_kib >= 8u * lanes && memory_kib <= max_memory_kib;
  }

  bool operator==(const Argon2Costs&) const = default;
};

// The tag, `length` bytes in secure memory. A salt is at least 8 bytes;
// `secret` and `associated` are Argon2's optional K and X. Throws
// std::invalid_argument for a short salt or costs that are not valid(),
// std::runtime_error if the computation fails. Waits on the CPU and memory
// it is told to spend, so it runs on a worker.
Secret argon2id(std::span<const uint8_t> password, std::span<const uint8_t> salt, const Argon2Costs& costs, size_t length = 32,
                std::span<const uint8_t> secret = {}, std::span<const uint8_t> associated = {});

}  // namespace dannan::identity
