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

#include <string>

#include "dannan/identity/signing.h"

namespace dannan::identity {
namespace {

Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

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
                         [](const testing::TestParamInfo<SigningAlgorithm>& info) { return std::string(algorithm_name(info.param)); });

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
