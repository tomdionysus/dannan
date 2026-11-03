//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/signing.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

#include <stdexcept>

namespace dannan::identity {
namespace {

using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using EcdsaSignature = std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)>;
using Pkcs8 = std::unique_ptr<PKCS8_PRIV_KEY_INFO, decltype(&PKCS8_PRIV_KEY_INFO_free)>;

// Which of ours an OpenSSL key is, if either.
std::optional<SigningAlgorithm> algorithm_of(EVP_PKEY* key) {
  if (EVP_PKEY_is_a(key, "ED25519")) return SigningAlgorithm::ed25519;
  if (EVP_PKEY_is_a(key, "EC")) {
    char group[64] = {};
    size_t length = 0;
    if (EVP_PKEY_get_utf8_string_param(key, "group", group, sizeof(group), &length) == 1 && std::string(group, length) == "prime256v1")
      return SigningAlgorithm::es256;
  }
  return std::nullopt;
}

// DER ECDSA (what OpenSSL signs) to and from 64 raw bytes.
Bytes der_to_raw(std::span<const uint8_t> der) {
  const unsigned char* cursor = der.data();
  EcdsaSignature signature(d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der.size())), ECDSA_SIG_free);
  if (!signature) throw std::runtime_error("ES256: malformed signature");
  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(signature.get(), &r, &s);
  Bytes out(64);
  if (BN_bn2binpad(r, out.data(), 32) != 32 || BN_bn2binpad(s, out.data() + 32, 32) != 32) throw std::runtime_error("ES256: signature out of range");
  return out;
}

std::optional<Bytes> raw_to_der(std::span<const uint8_t> raw) {
  if (raw.size() != 64) return std::nullopt;
  EcdsaSignature signature(ECDSA_SIG_new(), ECDSA_SIG_free);
  BIGNUM* r = BN_bin2bn(raw.data(), 32, nullptr);
  BIGNUM* s = BN_bin2bn(raw.data() + 32, 32, nullptr);
  if (!signature || !r || !s || ECDSA_SIG_set0(signature.get(), r, s) != 1) {
    BN_free(r);
    BN_free(s);
    return std::nullopt;
  }
  const int size = i2d_ECDSA_SIG(signature.get(), nullptr);
  if (size <= 0) return std::nullopt;
  Bytes out(static_cast<size_t>(size));
  unsigned char* cursor = out.data();
  if (i2d_ECDSA_SIG(signature.get(), &cursor) != size) return std::nullopt;
  return out;
}

const EVP_MD* digest_for(SigningAlgorithm algorithm) { return algorithm == SigningAlgorithm::es256 ? EVP_sha256() : nullptr; }

}  // namespace

struct SigningKey::Key {
  Pkey pkey{nullptr, EVP_PKEY_free};
};

const char* algorithm_name(SigningAlgorithm algorithm) noexcept {
  switch (algorithm) {
    case SigningAlgorithm::es256:
      return "ES256";
    case SigningAlgorithm::ed25519:
      return "EdDSA";
  }
  return "unknown";
}

SigningKey SigningKey::generate(SigningAlgorithm algorithm) {
  EVP_PKEY* raw = algorithm == SigningAlgorithm::es256 ? EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256") : EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519");
  if (!raw) throw std::runtime_error(std::string(algorithm_name(algorithm)) + ": key generation failed");
  auto key = std::make_shared<Key>();
  key->pkey.reset(raw);
  return SigningKey(algorithm, std::move(key));
}

SigningKey SigningKey::from_pkcs8(std::span<const uint8_t> der) {
  const unsigned char* cursor = der.data();
  Pkcs8 info(d2i_PKCS8_PRIV_KEY_INFO(nullptr, &cursor, static_cast<long>(der.size())), PKCS8_PRIV_KEY_INFO_free);
  if (!info || cursor != der.data() + der.size()) throw std::runtime_error("not a PKCS#8 private key");
  auto key = std::make_shared<Key>();
  key->pkey.reset(EVP_PKCS82PKEY(info.get()));
  if (!key->pkey) throw std::runtime_error("not a PKCS#8 private key");
  const auto algorithm = algorithm_of(key->pkey.get());
  if (!algorithm) throw std::runtime_error("a PKCS#8 key of neither ES256 nor Ed25519");
  return SigningKey(*algorithm, std::move(key));
}

Secret SigningKey::to_pkcs8() const {
  Pkcs8 info(EVP_PKEY2PKCS8(_key->pkey.get()), PKCS8_PRIV_KEY_INFO_free);
  if (!info) throw std::runtime_error("PKCS#8 encoding failed");
  const int size = i2d_PKCS8_PRIV_KEY_INFO(info.get(), nullptr);
  if (size <= 0) throw std::runtime_error("PKCS#8 encoding failed");
  SecureBytes out(static_cast<size_t>(size));
  unsigned char* cursor = out.data();
  if (i2d_PKCS8_PRIV_KEY_INFO(info.get(), &cursor) != size) throw std::runtime_error("PKCS#8 encoding failed");
  return Secret(std::move(out));
}

Bytes SigningKey::public_key() const {
  const int size = i2d_PUBKEY(_key->pkey.get(), nullptr);
  if (size <= 0) throw std::runtime_error("public key encoding failed");
  Bytes out(static_cast<size_t>(size));
  unsigned char* cursor = out.data();
  if (i2d_PUBKEY(_key->pkey.get(), &cursor) != size) throw std::runtime_error("public key encoding failed");
  return out;
}

Bytes SigningKey::sign(std::span<const uint8_t> message) const {
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestSignInit(context.get(), nullptr, digest_for(_algorithm), nullptr, _key->pkey.get()) != 1)
    throw std::runtime_error(std::string(algorithm_name(_algorithm)) + ": sign init failed");
  size_t size = 0;
  if (EVP_DigestSign(context.get(), nullptr, &size, message.data(), message.size()) != 1) throw std::runtime_error("sign failed");
  Bytes out(size);
  if (EVP_DigestSign(context.get(), out.data(), &size, message.data(), message.size()) != 1) throw std::runtime_error("sign failed");
  out.resize(size);
  if (_algorithm == SigningAlgorithm::es256) return der_to_raw(out);
  if (out.size() != 64) throw std::runtime_error("Ed25519: unexpected signature size");
  return out;
}

bool verify(SigningAlgorithm algorithm, std::span<const uint8_t> public_key, std::span<const uint8_t> message, std::span<const uint8_t> signature) noexcept {
  if (signature.size() != 64) return false;
  const unsigned char* cursor = public_key.data();
  Pkey key(d2i_PUBKEY(nullptr, &cursor, static_cast<long>(public_key.size())), EVP_PKEY_free);
  if (!key || cursor != public_key.data() + public_key.size()) return false;
  const auto actual = algorithm_of(key.get());
  if (!actual || *actual != algorithm) return false;
  Bytes der;
  std::span<const uint8_t> checked = signature;
  if (algorithm == SigningAlgorithm::es256) {
    auto converted = raw_to_der(signature);
    if (!converted) return false;
    der = std::move(*converted);
    checked = der;
  }
  DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestVerifyInit(context.get(), nullptr, digest_for(algorithm), nullptr, key.get()) != 1) return false;
  return EVP_DigestVerify(context.get(), checked.data(), checked.size(), message.data(), message.size()) == 1;
}

}  // namespace dannan::identity
