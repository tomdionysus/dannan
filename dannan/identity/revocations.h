//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/certificate.h"

namespace dannan::identity {

// The revocations a node holds (TODO/2026-11-04-identity.md, section 3):
// grow-only, each signed by the cluster's CA and checked before it is kept,
// persisted under the state path (DNNREV01) so a restart forgets none.
// Thread-safe; add() waits on the file. Lock order: _persist_mutex before
// _mutex; revoked() and all() take only _mutex, so a check never waits on
// the file.
class RevocationStore {
 public:
  // `path` empty: nothing persisted. A file that cannot be read, or holds a
  // revocation the CA did not sign, is refused (std::runtime_error): a
  // revocation silently lost is a node silently let back in.
  RevocationStore(Certificate ca, std::filesystem::path path = {});

  // Keeps those the CA signed and this store did not hold; the ones kept,
  // for the caller to act on (close the revoked node's sessions) and pass
  // on. Persists before returning when any was kept.
  std::vector<Revocation> add(const std::vector<Revocation>& revocations);
  bool revoked(const Certificate& certificate) const;
  std::vector<Revocation> all() const;

 private:
  const Certificate _ca;
  const std::filesystem::path _path;
  // Orders whole adds, file write included.
  contract::IoMutex _persist_mutex;
  mutable contract::Mutex _mutex;
  std::vector<Revocation> _held DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::identity
