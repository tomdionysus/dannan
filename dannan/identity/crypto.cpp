//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/crypto.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <stdexcept>

namespace dannan::identity {
namespace {

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

int length(size_t size) {
  if (size > static_cast<size_t>(INT32_MAX)) throw std::runtime_error("buffer too large for the cipher");
  return static_cast<int>(size);
}

}  // namespace

struct Sha256Hasher::State {
  DigestContext context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
  bool finished = false;
  State() {
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) throw std::runtime_error("SHA-256 init failed");
  }
};

Sha256Hasher::Sha256Hasher() : _state(std::make_unique<State>()) {}
Sha256Hasher::~Sha256Hasher() = default;
Sha256Hasher::Sha256Hasher(Sha256Hasher&&) noexcept = default;
Sha256Hasher& Sha256Hasher::operator=(Sha256Hasher&&) noexcept = default;

void Sha256Hasher::update(std::span<const uint8_t> data) {
  if (!_state || _state->finished) throw std::runtime_error("SHA-256 context already finished");
  if (!data.empty() && EVP_DigestUpdate(_state->context.get(), data.data(), data.size()) != 1) throw std::runtime_error("SHA-256 update failed");
}

Hash256 Sha256Hasher::finish() {
  if (!_state || _state->finished) throw std::runtime_error("SHA-256 context already finished");
  Hash256 out;
  unsigned size = 0;
  if (EVP_DigestFinal_ex(_state->context.get(), out.bytes.data(), &size) != 1 || size != 32) throw std::runtime_error("SHA-256 final failed");
  _state->finished = true;
  return out;
}

Hash256 sha256(std::span<const uint8_t> data) {
  Sha256Hasher hasher;
  hasher.update(data);
  return hasher.finish();
}

std::array<uint8_t, 32> hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> data) {
  std::array<uint8_t, 32> out{};
  unsigned size = 0;
  if (!HMAC(EVP_sha256(), key.data(), length(key.size()), data.data(), data.size(), out.data(), &size) || size != 32) throw std::runtime_error("HMAC failed");
  return out;
}

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
  std::array<uint8_t, 20> out{};
  unsigned size = 0;
  if (EVP_Digest(data.data(), data.size(), out.data(), &size, EVP_sha1(), nullptr) != 1 || size != 20) throw std::runtime_error("SHA-1 failed");
  return out;
}

std::array<uint8_t, 20> hmac_sha1(std::span<const uint8_t> key, std::span<const uint8_t> data) {
  std::array<uint8_t, 20> out{};
  unsigned size = 0;
  if (!HMAC(EVP_sha1(), key.data(), length(key.size()), data.data(), data.size(), out.data(), &size) || size != 20) throw std::runtime_error("HMAC failed");
  return out;
}

namespace {

// HKDF-SHA256 into `out`, whatever memory it is.
void hkdf_into(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info, std::span<uint8_t> out) {
  KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_derive_init(context.get()) <= 0 || EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(context.get(), key.data(), length(key.size())) <= 0)
    throw std::runtime_error("HKDF init failed");
  if (!salt.empty() && EVP_PKEY_CTX_set1_hkdf_salt(context.get(), salt.data(), length(salt.size())) <= 0) throw std::runtime_error("HKDF salt failed");
  if (!info.empty() && EVP_PKEY_CTX_add1_hkdf_info(context.get(), info.data(), length(info.size())) <= 0) throw std::runtime_error("HKDF info failed");
  size_t size = out.size();
  if (EVP_PKEY_derive(context.get(), out.data(), &size) <= 0 || size != out.size()) throw std::runtime_error("HKDF derive failed");
}

}  // namespace

std::array<uint8_t, 32> hkdf_sha256(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info) {
  std::array<uint8_t, 32> out{};
  hkdf_into(key, salt, info, out);
  return out;
}

Secret hkdf_sha256_secret(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info) {
  SecureBytes out(32);
  hkdf_into(key, salt, info, out.span());
  return Secret(std::move(out));
}

namespace {

std::array<uint8_t, 12> random_nonce() {
  std::array<uint8_t, 12> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) throw std::runtime_error("random nonce failed");
  return nonce;
}

// `out` is exactly `in`'s size, and either `in` itself or apart from it: the cipher
// works in place, never across a partial overlap.
void exact_or_apart(std::span<const uint8_t> in, std::span<uint8_t> out) {
  if (out.size() != in.size()) throw std::invalid_argument("AES-GCM output is not the input's size");
  if (in.empty() || out.data() == in.data()) return;
  const auto* a = in.data();
  const auto* b = out.data();
  if (a < b + out.size() && b < a + in.size()) throw std::invalid_argument("AES-GCM output partly overlaps its input");
}

// AES-256-GCM over `in` into `out`, which is `in`'s size: GCM produces as many bytes as
// it takes, and nothing more at the end.
GcmSeal seal_core(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t> in, uint8_t* out,
                  std::span<const uint8_t> associated) {
  GcmSeal seal;
  std::copy(nonce.begin(), nonce.end(), seal.nonce.begin());
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), seal.nonce.data()) != 1)
    throw std::runtime_error("AES-GCM init failed");
  int produced = 0;
  if (!associated.empty() && EVP_EncryptUpdate(context.get(), nullptr, &produced, associated.data(), length(associated.size())) != 1)
    throw std::runtime_error("AES-GCM associated data failed");
  int total = 0;
  if (!in.empty()) {
    if (EVP_EncryptUpdate(context.get(), out, &produced, in.data(), length(in.size())) != 1) throw std::runtime_error("AES-GCM encrypt failed");
    total = produced;
  }
  std::array<uint8_t, 16> tail{};
  if (EVP_EncryptFinal_ex(context.get(), in.empty() ? tail.data() : out + total, &produced) != 1) throw std::runtime_error("AES-GCM final failed");
  if (static_cast<size_t>(total + produced) != in.size()) throw std::runtime_error("AES-GCM produced the wrong length");
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, 16, seal.tag.data()) != 1) throw std::runtime_error("AES-GCM tag failed");
  return seal;
}

// The open's counterpart: throws when the message does not authenticate, `out` then
// holding unauthenticated bytes for the caller to clear.
void open_core(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> in,
               uint8_t* out, std::span<const uint8_t> associated) {
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
    throw std::runtime_error("AES-GCM init failed");
  int produced = 0;
  if (!associated.empty() && EVP_DecryptUpdate(context.get(), nullptr, &produced, associated.data(), length(associated.size())) != 1)
    throw std::runtime_error("AES-GCM associated data failed");
  int total = 0;
  if (!in.empty()) {
    if (EVP_DecryptUpdate(context.get(), out, &produced, in.data(), length(in.size())) != 1) throw std::runtime_error("AES-GCM decrypt failed");
    total = produced;
  }
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag.data())) != 1) throw std::runtime_error("AES-GCM tag failed");
  std::array<uint8_t, 16> tail{};
  if (EVP_DecryptFinal_ex(context.get(), in.empty() ? tail.data() : out + total, &produced) != 1) throw std::runtime_error("AES-GCM authentication failed");
  if (static_cast<size_t>(total + produced) != in.size()) throw std::runtime_error("AES-GCM produced the wrong length");
}

// Open into a new buffer of the ciphertext's size, whatever memory it is.
template <class Buffer>
Buffer open_into(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext,
                 std::span<const uint8_t> associated) {
  Buffer out(ciphertext.size());
  open_core(key, nonce, tag, ciphertext, out.data(), associated);
  return out;
}

}  // namespace

SealedData aes_gcm_seal(std::span<const uint8_t, 32> key, std::span<const uint8_t> plaintext, std::span<const uint8_t> associated) {
  return aes_gcm_seal(key, random_nonce(), plaintext, associated);
}

SealedData aes_gcm_seal(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t> plaintext,
                        std::span<const uint8_t> associated) {
  SealedData out;
  out.ciphertext.resize(plaintext.size());
  const auto seal = seal_core(key, nonce, plaintext, out.ciphertext.data(), associated);
  out.nonce = seal.nonce;
  out.tag = seal.tag;
  return out;
}

Bytes aes_gcm_open(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext,
                   std::span<const uint8_t> associated) {
  return open_into<Bytes>(key, nonce, tag, ciphertext, associated);
}

SecureBuffer aes_gcm_open_secure(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                                 std::span<const uint8_t> ciphertext, std::span<const uint8_t> associated) {
  return open_into<SecureBuffer>(key, nonce, tag, ciphertext, associated);
}

GcmSeal aes_gcm_seal_into(std::span<const uint8_t, 32> key, std::span<const uint8_t> plaintext, std::span<uint8_t> out, std::span<const uint8_t> associated) {
  return aes_gcm_seal_into(key, random_nonce(), plaintext, out, associated);
}

GcmSeal aes_gcm_seal_into(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t> plaintext, std::span<uint8_t> out,
                          std::span<const uint8_t> associated) {
  exact_or_apart(plaintext, out);
  return seal_core(key, nonce, plaintext, out.data(), associated);
}

void aes_gcm_open_into(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag,
                       std::span<const uint8_t> ciphertext, std::span<uint8_t> out, std::span<const uint8_t> associated) {
  exact_or_apart(ciphertext, out);
  try {
    open_core(key, nonce, tag, ciphertext, out.data(), associated);
  } catch (...) {
    // Nothing unauthenticated is left behind, in place or not.
    if (!out.empty()) OPENSSL_cleanse(out.data(), out.size());
    throw;
  }
}

X25519KeyPair x25519_generate() {
  KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) <= 0) throw std::runtime_error("X25519 keygen init failed");
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw) <= 0) throw std::runtime_error("X25519 keygen failed");
  Key key(raw, EVP_PKEY_free);
  X25519KeyPair out;
  size_t private_size = out.private_key.size();
  size_t public_size = out.public_key.size();
  if (EVP_PKEY_get_raw_private_key(key.get(), out.private_key.data(), &private_size) <= 0 ||
      EVP_PKEY_get_raw_public_key(key.get(), out.public_key.data(), &public_size) <= 0 || private_size != 32 || public_size != 32)
    throw std::runtime_error("X25519 key export failed");
  return out;
}

std::array<uint8_t, 32> x25519_public(std::span<const uint8_t, 32> private_key) {
  Key key(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), private_key.size()), EVP_PKEY_free);
  if (!key) throw std::runtime_error("X25519 key import failed");
  std::array<uint8_t, 32> out{};
  size_t size = out.size();
  if (EVP_PKEY_get_raw_public_key(key.get(), out.data(), &size) <= 0 || size != out.size()) throw std::runtime_error("X25519 public key export failed");
  return out;
}

std::array<uint8_t, 32> x25519_shared(std::span<const uint8_t, 32> private_key, std::span<const uint8_t, 32> peer_public_key) {
  Key local(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), private_key.size()), EVP_PKEY_free);
  Key peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_public_key.data(), peer_public_key.size()), EVP_PKEY_free);
  if (!local || !peer) throw std::runtime_error("X25519 key import failed");
  KeyContext context(EVP_PKEY_CTX_new(local.get(), nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_derive_init(context.get()) <= 0 || EVP_PKEY_derive_set_peer(context.get(), peer.get()) <= 0)
    throw std::runtime_error("X25519 derive init failed");
  std::array<uint8_t, 32> out{};
  size_t size = out.size();
  if (EVP_PKEY_derive(context.get(), out.data(), &size) <= 0 || size != out.size()) throw std::runtime_error("X25519 derive failed");
  return out;
}

Bytes random_bytes(size_t count) {
  Bytes out(count);
  if (count && RAND_bytes(out.data(), length(count)) != 1) throw std::runtime_error("RAND_bytes failed");
  return out;
}

bool constant_time_equal(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

void cleanse(std::span<uint8_t> bytes) noexcept { OPENSSL_cleanse(bytes.data(), bytes.size()); }

}  // namespace dannan::identity
