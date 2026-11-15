//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Signing, the stack's one home for it: ES256 (ECDSA over P-256 with
// SHA-256, signatures as 64 raw bytes r || s, as JOSE and compact formats
// want them) and Ed25519. Private keys travel as PKCS#8 in secure memory,
// public keys as SubjectPublicKeyInfo. Step 10's cluster CA and Lugh's
// token signing both use these.

#include <gtest/gtest.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <memory>
#include <string>

#include "dannan/identity/signing.h"

namespace dannan::identity {
namespace {

Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

// An RSA key made here, its modulus and exponent as WebAuthn's COSE gives them, and
// signatures under it with PKCS#1 v1.5 or PSS.
struct RsaKey {
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key;
  Bytes n, e;

  explicit RsaKey(unsigned bits) : key(EVP_RSA_gen(bits), EVP_PKEY_free) {
    BIGNUM* modulus = nullptr;
    BIGNUM* exponent = nullptr;
    EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_RSA_N, &modulus);
    EVP_PKEY_get_bn_param(key.get(), OSSL_PKEY_PARAM_RSA_E, &exponent);
    n.resize(static_cast<size_t>(BN_num_bytes(modulus)));
    e.resize(static_cast<size_t>(BN_num_bytes(exponent)));
    BN_bn2bin(modulus, n.data());
    BN_bn2bin(exponent, e.data());
    BN_free(modulus);
    BN_free(exponent);
  }

  Bytes sign(const Bytes& message, int padding = RSA_PKCS1_PADDING) const {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* settings = nullptr;
    EVP_DigestSignInit(context.get(), &settings, EVP_sha256(), nullptr, key.get());
    EVP_PKEY_CTX_set_rsa_padding(settings, padding);
    size_t size = 0;
    EVP_DigestSign(context.get(), nullptr, &size, message.data(), message.size());
    Bytes out(size);
    EVP_DigestSign(context.get(), out.data(), &size, message.data(), message.size());
    out.resize(size);
    return out;
  }
};

// RS256 verifies a PKCS#1 v1.5 SHA-256 signature under (n, e) as COSE carries them,
// and nothing else: another message, a bent signature, PSS, a modulus with a leading
// zero byte still the same key.
TEST(Rs256, VerifiesWhatItsKeySignedAndNothingElse) {
  const RsaKey key(2048);
  const auto message = text("webauthn assertion");
  const auto signature = key.sign(message);
  EXPECT_TRUE(verify_rs256(key.n, key.e, message, signature));
  EXPECT_FALSE(verify_rs256(key.n, key.e, text("another assertion"), signature));
  auto bent = signature;
  bent[100] ^= 1;
  EXPECT_FALSE(verify_rs256(key.n, key.e, message, bent));
  EXPECT_FALSE(verify_rs256(key.n, key.e, message, key.sign(message, RSA_PKCS1_PSS_PADDING)));
  Bytes padded(1, 0);
  padded.insert(padded.end(), key.n.begin(), key.n.end());
  EXPECT_TRUE(verify_rs256(padded, key.e, message, signature));
  const RsaKey other(2048);
  EXPECT_FALSE(verify_rs256(other.n, other.e, message, signature));
}

// The bounds Lugh asked for, and malformed input: false, never a throw.
TEST(Rs256, RefusesWeakKeysAndMalformedInput) {
  const RsaKey small(1024);
  const auto message = text("m");
  EXPECT_FALSE(verify_rs256(small.n, small.e, message, small.sign(message)));
  const RsaKey key(2048);
  const auto signature = key.sign(message);
  EXPECT_FALSE(verify_rs256(key.n, Bytes{4}, message, signature));
  EXPECT_FALSE(verify_rs256(key.n, Bytes{1}, message, signature));
  EXPECT_FALSE(verify_rs256(key.n, Bytes{}, message, signature));
  EXPECT_FALSE(verify_rs256({}, key.e, message, signature));
  EXPECT_FALSE(verify_rs256(key.n, key.e, message, Bytes(signature.begin(), signature.end() - 1)));
  EXPECT_FALSE(verify_rs256(key.n, key.e, message, {}));
  EXPECT_FALSE(verify_rs256(Bytes(1100, 0xff), key.e, message, signature));
  auto even = key.n;
  even.back() &= 0xfe;
  EXPECT_FALSE(verify_rs256(even, key.e, message, signature));
}

class Signing : public testing::TestWithParam<SigningAlgorithm> {};

TEST_P(Signing, SignsAndVerifies) {
  const auto key = SigningKey::generate(GetParam());
  EXPECT_EQ(key.algorithm(), GetParam());
  const auto message = text("the directory changed");
  const auto signature = key.sign(message);
  EXPECT_EQ(signature.size(), 64u);
  EXPECT_TRUE(verify(GetParam(), key.public_key(), message, signature));
  EXPECT_FALSE(verify(GetParam(), key.public_key(), text("the directory changed!"), signature));
  auto bent = signature;
  bent[10] ^= 1;
  EXPECT_FALSE(verify(GetParam(), key.public_key(), message, bent));
  EXPECT_FALSE(verify(GetParam(), SigningKey::generate(GetParam()).public_key(), message, signature));
}

// The private key leaves only as PKCS#8, in secure memory, and comes back
// as the same key.
TEST_P(Signing, RoundTripsThroughPkcs8) {
  const auto key = SigningKey::generate(GetParam());
  const auto der = key.to_pkcs8();
  const auto again = SigningKey::from_pkcs8(der.reveal());
  EXPECT_EQ(again.algorithm(), GetParam());
  EXPECT_EQ(again.public_key(), key.public_key());
  const auto message = text("m");
  EXPECT_TRUE(verify(GetParam(), key.public_key(), message, again.sign(message)));
}

// Garbage, or a key of the other algorithm, is refused, never guessed.
TEST_P(Signing, RefusesWhatIsNotItsKey) {
  EXPECT_THROW(SigningKey::from_pkcs8(text("not a key")), std::runtime_error);
  const auto key = SigningKey::generate(GetParam());
  const auto other = GetParam() == SigningAlgorithm::es256 ? SigningAlgorithm::ed25519 : SigningAlgorithm::es256;
  const auto message = text("m");
  EXPECT_FALSE(verify(other, key.public_key(), message, key.sign(message)));
  EXPECT_FALSE(verify(GetParam(), text("not a public key"), message, key.sign(message)));
  EXPECT_FALSE(verify(GetParam(), key.public_key(), message, Bytes(63, 0)));
}

INSTANTIATE_TEST_SUITE_P(Algorithms, Signing, testing::Values(SigningAlgorithm::es256, SigningAlgorithm::ed25519),
                         [](const testing::TestParamInfo<SigningAlgorithm>& instance) { return std::string(algorithm_name(instance.param)); });

// ES256 is deterministic in its verification only: two signatures of one
// message differ (a fresh nonce each), both verify.
TEST(SigningEs256, SignaturesAreFreshEachTime) {
  const auto key = SigningKey::generate(SigningAlgorithm::es256);
  const auto message = text("m");
  const auto a = key.sign(message);
  const auto b = key.sign(message);
  EXPECT_NE(a, b);
  EXPECT_TRUE(verify(SigningAlgorithm::es256, key.public_key(), message, a));
  EXPECT_TRUE(verify(SigningAlgorithm::es256, key.public_key(), message, b));
}

TEST(SigningNames, AreJosesNames) {
  EXPECT_STREQ(algorithm_name(SigningAlgorithm::es256), "ES256");
  EXPECT_STREQ(algorithm_name(SigningAlgorithm::ed25519), "EdDSA");
}

}  // namespace
}  // namespace dannan::identity
