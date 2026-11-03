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
  ClusterKey key;
  key.master = sha256(secret).bytes;
  cleanse(secret);
  key.auth = key.derive("macha/auth/v1");
  const auto id = key.derive("macha/cluster-id/v1");
  std::copy_n(id.begin(), key.cluster_id.size(), key.cluster_id.begin());
  return key;
}

}  // namespace dannan::identity
