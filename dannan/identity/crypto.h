//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The primitives the channel seals and authenticates with, over OpenSSL.
// Every failure throws std::runtime_error; a failed open or verify is never
// a value a caller could mistake for success.
#include "dannan/encoding/base.h"
#include "dannan/identity/secure_memory.h"

namespace dannan::identity {

using Bytes = std::vector<uint8_t>;

struct Hash256 {
  std::array<uint8_t, 32> bytes{};
  auto operator<=>(const Hash256&) const = default;
};

struct SealedData {
  std::array<uint8_t, 12> nonce{};
  std::array<uint8_t, 16> tag{};
  Bytes ciphertext;
};

struct X25519KeyPair {
  std::array<uint8_t, 32> private_key{};
  std::array<uint8_t, 32> public_key{};
};

class Sha256Hasher {
 public:
  Sha256Hasher();
  ~Sha256Hasher();
  Sha256Hasher(Sha256Hasher&&) noexcept;
  Sha256Hasher& operator=(Sha256Hasher&&) noexcept;
  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;
  void update(std::span<const uint8_t>);
  Hash256 finish();

 private:
  struct State;
  std::unique_ptr<State> _state;
};

Hash256 sha256(std::span<const uint8_t>);
std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> data);
// SHA-1 and HMAC-SHA-1 (TODO/2026-11-20-stack-plan.md, item 12), for formats that fix them: a
// Valkey script's id (Mimir), TOTP (RFC 6238, Lugh). Never for a new design: SHA-1 is broken for
// collisions.
std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);
std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data);
// HKDF-SHA256 (RFC 5869), 32 bytes of output. An empty salt is no salt.
std::array<uint8_t, 32> hkdf_sha256(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info);
// As above, into secure memory: for a key that must never sit in ordinary
// memory (a key-encryption key, a derived domain key).
Secret hkdf_sha256_secret(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info);
// AES-256-GCM with a fresh random 12-byte nonce and a 16-byte tag.
SealedData aes_gcm_seal(std::span<const uint8_t, 32> key, std::span<const uint8_t> plaintext, std::span<const uint8_t> associated = {});
// As above under the nonce given. A nonce must never be used twice with one
// key: this exists to reproduce another implementation's frame in a test.
SealedData aes_gcm_seal(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t> plaintext,
                        std::span<const uint8_t> associated = {});
Bytes aes_gcm_open(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext,
                   std::span<const uint8_t> associated = {});
// As above, the plaintext into secure memory: for a record holding secrets.
SecureBuffer aes_gcm_open_secure(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                                 std::span<const uint8_t> ciphertext, std::span<const uint8_t> associated = {});
X25519KeyPair x25519_generate();
// The public half of an X25519 private key (RFC 7748: the scalar times the
// base point): for a key derived rather than generated.
std::array<uint8_t, 32> x25519_public(std::span<const uint8_t, 32> private_key);
std::array<uint8_t, 32> x25519_shared(std::span<const uint8_t, 32> private_key, std::span<const uint8_t, 32> peer_public_key);
Bytes random_bytes(size_t count);
bool constant_time_equal(std::span<const uint8_t>, std::span<const uint8_t>);
// Overwrites memory the optimiser may not elide.
void cleanse(std::span<uint8_t>) noexcept;

// hex and unhex live in dannan/encoding/base.h (the plan's item 9); these names forward to them
// for one minor.
using encoding::hex;
using encoding::unhex;
inline std::string hex(const Hash256& hash) { return encoding::hex(hash.bytes); }

}  // namespace dannan::identity
