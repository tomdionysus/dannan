//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/revocations.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "dannan/identity/durable_file.h"

namespace dannan::identity {

RevocationStore::RevocationStore(Certificate ca, std::filesystem::path path) : _ca(std::move(ca)), _path(std::move(path)) {
  if (_path.empty() || !std::filesystem::exists(_path)) return;
  std::ifstream input(_path, std::ios::binary);
  const Bytes bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::vector<Revocation> loaded;
  try {
    loaded = decode_revocations(bytes);
  } catch (const net::DecodeError& error) {
    throw std::runtime_error("the revocations at " + _path.string() + " cannot be read: " + error.what());
  }
  for (const auto& revocation : loaded)
    if (!verify_revocation(revocation, _ca)) throw std::runtime_error("the revocations at " + _path.string() + " hold one this cluster's CA did not sign");
  contract::Lock lock(_mutex);
  _held = std::move(loaded);
}

std::vector<Revocation> RevocationStore::add(const std::vector<Revocation>& revocations) {
  contract::Lock persisting(_persist_mutex);
  std::vector<Revocation> kept;
  Bytes encoded;
  {
    contract::Lock lock(_mutex);
    for (const auto& revocation : revocations) {
      if (std::find(_held.begin(), _held.end(), revocation) != _held.end()) continue;
      if (!verify_revocation(revocation, _ca)) continue;
      _held.push_back(revocation);
      kept.push_back(revocation);
    }
    if (kept.empty() || _path.empty()) return kept;
    encoded = encode_revocations(_held);
  }
  // Written under the persist lock: a later add holds every earlier one.
  durable_replace_file(_path, std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
  return kept;
}

bool RevocationStore::revoked(const Certificate& certificate) const {
  contract::Lock lock(_mutex);
  return std::any_of(_held.begin(), _held.end(), [&](const Revocation& r) { return r.covers(certificate); });
}

std::vector<Revocation> RevocationStore::all() const {
  contract::Lock lock(_mutex);
  return _held;
}

}  // namespace dannan::identity
