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

std::array<uint8_t, 32> hkdf_sha256(std::span<const uint8_t> key, std::span<const uint8_t> salt, std::span<const uint8_t> info) {
  std::array<uint8_t, 32> out{};
  KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_derive_init(context.get()) <= 0 || EVP_PKEY_CTX_set_hkdf_md(context.get(), EVP_sha256()) <= 0 ||
      EVP_PKEY_CTX_set1_hkdf_key(context.get(), key.data(), length(key.size())) <= 0)
    throw std::runtime_error("HKDF init failed");
  if (!salt.empty() && EVP_PKEY_CTX_set1_hkdf_salt(context.get(), salt.data(), length(salt.size())) <= 0) throw std::runtime_error("HKDF salt failed");
  if (!info.empty() && EVP_PKEY_CTX_add1_hkdf_info(context.get(), info.data(), length(info.size())) <= 0) throw std::runtime_error("HKDF info failed");
  size_t size = out.size();
  if (EVP_PKEY_derive(context.get(), out.data(), &size) <= 0 || size != out.size()) throw std::runtime_error("HKDF derive failed");
  return out;
}

SealedData aes_gcm_seal(std::span<const uint8_t, 32> key, std::span<const uint8_t> plaintext, std::span<const uint8_t> associated) {
  SealedData out;
  if (RAND_bytes(out.nonce.data(), static_cast<int>(out.nonce.size())) != 1) throw std::runtime_error("random nonce failed");
  out.ciphertext.resize(plaintext.size());
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), out.nonce.data()) != 1)
    throw std::runtime_error("AES-GCM init failed");
  int produced = 0;
  if (!associated.empty() && EVP_EncryptUpdate(context.get(), nullptr, &produced, associated.data(), length(associated.size())) != 1)
    throw std::runtime_error("AES-GCM associated data failed");
  int total = 0;
  if (!plaintext.empty()) {
    if (EVP_EncryptUpdate(context.get(), out.ciphertext.data(), &produced, plaintext.data(), length(plaintext.size())) != 1)
      throw std::runtime_error("AES-GCM encrypt failed");
    total = produced;
  }
  std::array<uint8_t, 16> tail{};
  if (EVP_EncryptFinal_ex(context.get(), out.ciphertext.empty() ? tail.data() : out.ciphertext.data() + total, &produced) != 1)
    throw std::runtime_error("AES-GCM final failed");
  out.ciphertext.resize(static_cast<size_t>(total + produced));
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, 16, out.tag.data()) != 1) throw std::runtime_error("AES-GCM tag failed");
  return out;
}

Bytes aes_gcm_open(std::span<const uint8_t, 32> key, std::span<const uint8_t, 12> nonce, std::span<const uint8_t, 16> tag, std::span<const uint8_t> ciphertext,
                   std::span<const uint8_t> associated) {
  Bytes out(ciphertext.size());
  CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1 ||
      EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
    throw std::runtime_error("AES-GCM init failed");
  int produced = 0;
  if (!associated.empty() && EVP_DecryptUpdate(context.get(), nullptr, &produced, associated.data(), length(associated.size())) != 1)
    throw std::runtime_error("AES-GCM associated data failed");
  int total = 0;
  if (!ciphertext.empty()) {
    if (EVP_DecryptUpdate(context.get(), out.data(), &produced, ciphertext.data(), length(ciphertext.size())) != 1)
      throw std::runtime_error("AES-GCM decrypt failed");
    total = produced;
  }
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag.data())) != 1) throw std::runtime_error("AES-GCM tag failed");
  std::array<uint8_t, 16> tail{};
  if (EVP_DecryptFinal_ex(context.get(), out.empty() ? tail.data() : out.data() + total, &produced) != 1)
    throw std::runtime_error("AES-GCM authentication failed");
  out.resize(static_cast<size_t>(total + produced));
  return out;
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

std::string hex(std::span<const uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 15]);
  }
  return out;
}

std::optional<Bytes> unhex(const std::string& text) {
  if (text.size() % 2) return std::nullopt;
  const auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Bytes out;
  out.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int high = value(text[i]);
    const int low = value(text[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<uint8_t>(high << 4 | low));
  }
  return out;
}

}  // namespace dannan::identity
