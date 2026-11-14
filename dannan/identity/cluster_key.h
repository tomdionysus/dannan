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

// The secret every node of a cluster shares, until certificates replace it
// (the degenerate cluster CA with one node class). Wire v1's derivation:
//
//   master      SHA-256 of the file, trailing CR and LF removed
//   auth        HKDF-SHA256(master, no salt, "dannan/auth/1")
//   cluster_id  the first 16 bytes of HKDF-SHA256(master, no salt,
//               "dannan/cluster-id/1")
//
// A hosted system derives its own keys from master under its own labels;
// Dannan names none of them. Its own two labels are named here, once: a host
// that holds master another way takes the keys from from_master() rather than
// repeating them, so a change of label cannot split a cluster.
inline constexpr std::string_view cluster_auth_label = "dannan/auth/1";
inline constexpr std::string_view cluster_id_label = "dannan/cluster-id/1";

struct ClusterKey {
  std::array<uint8_t, 32> master{};
  std::array<uint8_t, 32> auth{};
  std::array<uint8_t, 16> cluster_id{};

  // Auth and the cluster id, derived from `master` as the file's are.
  static ClusterKey from_master(const std::array<uint8_t, 32>& master);
  std::array<uint8_t, 32> derive(std::string_view label) const;
};

// Throws std::runtime_error when the file cannot be read or holds nothing
// but line ends. Waits on the state device.
ClusterKey load_cluster_key(const std::filesystem::path&);

}  // namespace dannan::identity
