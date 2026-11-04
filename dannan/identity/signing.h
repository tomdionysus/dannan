//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "dannan/identity/crypto.h"
#include "dannan/identity/secure_memory.h"

// Signatures, for the whole stack: step 10's cluster CA and node keys,
// Lugh's token signing keys, anything that must prove who wrote it. One
// home, so no project keeps its own (Tom, 2026-11-03).
struct evp_pkey_st;

namespace dannan::identity {

// ES256: ECDSA over P-256 with SHA-256. Ed25519: EdDSA over Curve25519.
// Both sign as 64 raw bytes (ES256: r then s, each 32 bytes big-endian, as
// JOSE wants it).
enum class SigningAlgorithm : uint8_t { es256, ed25519 };
// JOSE's names: "ES256", "EdDSA".
const char* algorithm_name(SigningAlgorithm) noexcept;

// A private signing key. Immutable once made; thread-safe for signing.
class SigningKey {
 public:
  // Throws std::runtime_error if the generator fails.
  static SigningKey generate(SigningAlgorithm algorithm);
  // From PKCS#8 (DER). Throws std::runtime_error for anything that is not
  // an ES256 or Ed25519 private key.
  static SigningKey from_pkcs8(std::span<const uint8_t> der);

  SigningAlgorithm algorithm() const noexcept { return _algorithm; }
  // PKCS#8 (DER), in secure memory: the only way the private key leaves.
  Secret to_pkcs8() const;
  // SubjectPublicKeyInfo (DER).
  Bytes public_key() const;
  // 64 bytes. Throws std::runtime_error if signing fails.
  Bytes sign(std::span<const uint8_t> message) const;

  // The OpenSSL key, for this library's own signing in a standard format
  // (X.509 certificates). Not for hosted systems.
  evp_pkey_st* native_handle() const noexcept;

 private:
  struct Key;
  SigningKey(SigningAlgorithm algorithm, std::shared_ptr<Key> key) : _algorithm(algorithm), _key(std::move(key)) {}

  SigningAlgorithm _algorithm;
  std::shared_ptr<Key> _key;
};

// Whether `signature` is `algorithm`'s signature of `message` under the
// SubjectPublicKeyInfo `public_key`. Malformed input is false, never an
// exception: whatever arrives from outside is checked here.
bool verify(SigningAlgorithm algorithm, std::span<const uint8_t> public_key, std::span<const uint8_t> message, std::span<const uint8_t> signature) noexcept;

// Which algorithm a SubjectPublicKeyInfo is for; nothing for anything else.
std::optional<SigningAlgorithm> algorithm_of_public_key(std::span<const uint8_t> public_key) noexcept;

}  // namespace dannan::identity
