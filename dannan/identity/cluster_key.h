//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace dannan::identity {

// The secret every node of a cluster shares, until protocol 25 replaces it
// with certificates: the degenerate cluster CA with one node class. Its
// derivation is protocol 23's, labels included, so a Dannan node and a
// Macha node holding the same file are one cluster:
//
//   master      SHA-256 of the file, trailing CR and LF removed
//   auth        HKDF-SHA256(master, no salt, "macha/auth/v1")
//   cluster_id  the first 16 bytes of HKDF-SHA256(master, no salt,
//               "macha/cluster-id/v1")
//
// A hosted system derives its own keys from master under its own labels
// (Dagda's storage key is "macha/storage/v1"); Dannan names none of them.
struct ClusterKey {
  std::array<uint8_t, 32> master{};
  std::array<uint8_t, 32> auth{};
  std::array<uint8_t, 16> cluster_id{};

  std::array<uint8_t, 32> derive(std::string_view label) const;
};

// Throws std::runtime_error when the file cannot be read or holds nothing
// but line ends. Waits on the state device.
ClusterKey load_cluster_key(const std::filesystem::path&);

}  // namespace dannan::identity
