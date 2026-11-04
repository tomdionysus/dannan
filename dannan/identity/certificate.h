//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/identity/node_id.h"
#include "dannan/identity/signing.h"
#include "dannan/net/codec.h"

struct x509_st;

// Certificates (TODO/2026-11-04-identity.md): X.509 through OpenSSL (Tom,
// 2026-11-04). A cluster is a CA: an EC P-256 key and a self-signed
// certificate, CA:TRUE with pathlen 0, kept offline and used through
// dannan-ca. A node is a certificate the CA issued over the node's own key
// (its custody), naming the node id in a subjectAltName URI
// "dannan:node:<32 hex digits>": the node id is the certificate's, never
// the address.
namespace dannan::identity {

inline constexpr std::chrono::hours default_node_validity{24 * 365 * 2};
inline constexpr std::chrono::hours default_ca_validity{24 * 365 * 10};

// An X.509 certificate, parsed. Immutable; cheap to copy.
class Certificate {
 public:
  // Throws std::runtime_error for anything that is not one certificate.
  static Certificate from_der(std::span<const uint8_t> der);
  static Certificate from_pem(std::string_view pem);

  Bytes der() const;
  std::string pem() const;
  // SubjectPublicKeyInfo (DER).
  Bytes public_key() const;
  // The node it names; nothing for a certificate that names none (a CA's).
  std::optional<NodeId> node_id() const;
  uint64_t serial() const;
  uint64_t not_before_unix_ms() const;
  uint64_t not_after_unix_ms() const;
  bool is_ca() const;
  // As "CN=name".
  std::string subject() const;
  // SHA-256 of the DER.
  Hash256 fingerprint() const;

  // This library's own checks (signatures) need it; not for hosted systems.
  x509_st* native_handle() const noexcept { return _x509.get(); }

 private:
  friend class CertificateAuthority;
  explicit Certificate(std::shared_ptr<x509_st> x509) : _x509(std::move(x509)) {}
  std::shared_ptr<x509_st> _x509;
};

// The cluster's id: the first 16 bytes of SHA-256 over the CA's
// SubjectPublicKeyInfo. Every node of one CA is one cluster.
std::array<uint8_t, 16> cluster_id_of(const Certificate& ca);

// Why `node` is not a certificate this cluster accepts at `now_unix_ms`
// (the receiver's clock): not issued by `ca`, out of its validity, a CA, or
// naming no node. Empty when it is accepted. Revocation is checked apart.
std::string check_node_certificate(const Certificate& node, const Certificate& ca, uint64_t now_unix_ms);

// A revocation the CA signed: the node, the certificate serial (zero: every
// certificate of that node), when and why. Grow-only, gossiped, checked at
// every certificate handshake.
struct Revocation {
  NodeId node;
  uint64_t serial = 0;
  uint64_t revoked_unix_ms = 0;
  std::string reason;
  Bytes signature;

  bool covers(const Certificate& certificate) const;
  bool operator==(const Revocation&) const = default;
};

// Whether `ca` signed it, unaltered.
bool verify_revocation(const Revocation& revocation, const Certificate& ca) noexcept;
// A set as gossiped and persisted (DNNREV01). Decoding throws
// net::DecodeError; it does not check signatures.
Bytes encode_revocations(const std::vector<Revocation>& revocations);
std::vector<Revocation> decode_revocations(std::span<const uint8_t> bytes);

// The CA: its key and certificate. Used by dannan-ca on an operator's
// machine; never held by a node.
class CertificateAuthority {
 public:
  // A new CA named `name`, valid from `now_unix_ms` for `validity`.
  static CertificateAuthority create(const std::string& name, uint64_t now_unix_ms, std::chrono::hours validity = default_ca_validity);
  // Throws std::invalid_argument for a key that is not the certificate's,
  // or a certificate that is not a CA's.
  CertificateAuthority(SigningKey key, Certificate certificate);

  const Certificate& certificate() const noexcept { return _certificate; }
  const SigningKey& key() const noexcept { return _key; }

  // A node certificate over `public_key` (SubjectPublicKeyInfo), valid from
  // `now_unix_ms`. Throws std::runtime_error for a key that is not one.
  Certificate issue(const NodeId& node, std::span<const uint8_t> public_key, uint64_t serial, uint64_t now_unix_ms,
                    std::chrono::hours validity = default_node_validity) const;
  Revocation revoke(const NodeId& node, uint64_t serial, uint64_t now_unix_ms, std::string reason) const;

 private:
  SigningKey _key;
  Certificate _certificate;
};

}  // namespace dannan::identity
