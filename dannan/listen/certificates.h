//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"

// The public side's certificates and TLS policy (P3). The public side never meets
// the cluster's CA or key: these are the operator's files for public clients.
namespace dannan::listen {

// Where a listener's certificates are, as the operator keeps them: the chain (leaf
// first) and its key, in PEM, and the PEM bundle client certificates are checked
// against (empty: none).
struct CertificatePaths {
  std::filesystem::path chain;
  std::filesystem::path key;
  std::filesystem::path client_ca;
};

enum class TlsVersion : uint8_t { tls12, tls13 };
// Whether a listener asks for a client certificate. `optional` asks every
// connection and never ends the handshake on the chain: the verdict is kept on the
// stream for the hosted system to judge (RFC 8705's certificate-bound tokens bind to
// any certificate, self-signed included). `required` ends it on a chain that does
// not verify. A browser-facing listener says `never`.
enum class ClientCertificates : uint8_t { never, optional, required };

struct TlsPolicy {
  TlsVersion minimum = TlsVersion::tls13;
  ClientCertificates client_certificates = ClientCertificates::never;
  // The protocols offered, in preference order ("http/1.1"); none: no ALPN.
  std::vector<std::string> alpn;
};

// The operator's certificate material, loaded and checked (the key is the chain's
// leaf's). One may serve several listeners, and one reload changes them all at their
// next handshake; connections already open keep what they began with. Thread-safe.
class Certificates {
 public:
  // What a load produced: opaque to the hosted system.
  struct Material;

  // Throws std::runtime_error naming the path that failed (never a value read from
  // it). Waits on the device.
  explicit Certificates(CertificatePaths paths);
  ~Certificates();
  Certificates(const Certificates&) = delete;
  Certificates& operator=(const Certificates&) = delete;

  // Loads the files again, the client bundle included. On failure keeps what it had
  // and throws std::runtime_error naming the path. Waits on the device.
  void reload();

  std::shared_ptr<const Material> current() const;
  // Moves on at each successful load.
  uint64_t generation() const;
  const CertificatePaths& paths() const noexcept { return _paths; }

 private:
  const CertificatePaths _paths;
  mutable contract::Mutex _mutex;
  std::shared_ptr<const Material> _material DANNAN_GUARDED_BY(_mutex);
  uint64_t _generation DANNAN_GUARDED_BY(_mutex) = 0;
};

}  // namespace dannan::listen
