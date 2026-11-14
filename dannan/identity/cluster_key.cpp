//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/cluster_key.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "dannan/identity/crypto.h"

namespace dannan::identity {

ClusterKey ClusterKey::from_master(const std::array<uint8_t, 32>& master) {
  ClusterKey key;
  key.master = master;
  key.auth = key.derive(cluster_auth_label);
  const auto id = key.derive(cluster_id_label);
  std::copy_n(id.begin(), key.cluster_id.size(), key.cluster_id.begin());
  return key;
}

std::array<uint8_t, 32> ClusterKey::derive(std::string_view label) const {
  return hkdf_sha256(master, {}, std::span(reinterpret_cast<const uint8_t*>(label.data()), label.size()));
}

ClusterKey load_cluster_key(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open cluster key file " + path.string());
  Bytes secret((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r')) secret.pop_back();
  if (secret.empty()) {
    throw std::runtime_error("cluster key file " + path.string() + " is empty");
  }
  const auto master = sha256(secret).bytes;
  cleanse(secret);
  return ClusterKey::from_master(master);
}

}  // namespace dannan::identity
