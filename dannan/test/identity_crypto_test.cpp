//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The primitives the channel and the store seal with, against their
// published test vectors, and the cluster key: the inventory's rows "the
// cluster key file; the channel keyed from it" and "a durable node id".

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>

#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/node_id.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::identity {
namespace {

Bytes from_hex(std::string_view text) { return *unhex(std::string(text)); }

template <size_t N>
std::array<uint8_t, N> array_hex(std::string_view text) {
  const auto bytes = from_hex(text);
  std::array<uint8_t, N> out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

TEST(Crypto, Sha256MatchesFips180) {
  const std::string abc = "abc";
  EXPECT_EQ(hex(sha256(std::span(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()))),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  Sha256Hasher hasher;
  hasher.update(std::span(reinterpret_cast<const uint8_t*>(abc.data()), 1));
  hasher.update(std::span(reinterpret_cast<const uint8_t*>(abc.data()) + 1, 2));
  EXPECT_EQ(hex(hasher.finish()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Crypto, HmacSha256MatchesRfc4231) {
  const Bytes key(20, 0x0b);
  const std::string data = "Hi There";
  EXPECT_EQ(hex(hmac_sha256(key, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()))),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

// The first 32 bytes of RFC 5869's first case.
TEST(Crypto, HkdfSha256MatchesRfc5869) {
  const Bytes ikm(22, 0x0b);
  const auto salt = from_hex("000102030405060708090a0b0c");
  const auto info = from_hex("f0f1f2f3f4f5f6f7f8f9");
  EXPECT_EQ(hex(hkdf_sha256(ikm, salt, info)), "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf");
}

TEST(Crypto, X25519MatchesRfc7748) {
  const auto alice_private = array_hex<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
  const auto bob_public = array_hex<32>("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
  EXPECT_EQ(hex(x25519_shared(alice_private, bob_public)), "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
}

TEST(Crypto, X25519KeyPairsAgree) {
  const auto alice = x25519_generate();
  const auto bob = x25519_generate();
  EXPECT_EQ(x25519_shared(alice.private_key, bob.public_key), x25519_shared(bob.private_key, alice.public_key));
}

TEST(Crypto, AesGcmOpensWhatItSealedWithTheSameAssociatedData) {
  const auto key = array_hex<32>("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const Bytes plain(1000, 0x5a);
  const Bytes aad{1, 2, 3};
  const auto sealed = aes_gcm_seal(key, plain, aad);
  EXPECT_EQ(sealed.ciphertext.size(), plain.size());
  EXPECT_EQ(aes_gcm_open(key, sealed.nonce, sealed.tag, sealed.ciphertext, aad), plain);
}

// An empty plaintext with associated data seals to an empty ciphertext.
TEST(Crypto, AesGcmSealsAnEmptyMessage) {
  const auto key = array_hex<32>("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const Bytes aad{1, 2, 3, 4, 5};
  const auto sealed = aes_gcm_seal(key, {}, aad);
  EXPECT_TRUE(sealed.ciphertext.empty());
  EXPECT_TRUE(aes_gcm_open(key, sealed.nonce, sealed.tag, sealed.ciphertext, aad).empty());
}

TEST(Crypto, AesGcmRefusesAnyAlteration) {
  const auto key = array_hex<32>("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  const Bytes plain{1, 2, 3, 4};
  const Bytes aad{9};
  auto sealed = aes_gcm_seal(key, plain, aad);
  auto bad_text = sealed;
  bad_text.ciphertext[0] ^= 1;
  EXPECT_THROW(aes_gcm_open(key, bad_text.nonce, bad_text.tag, bad_text.ciphertext, aad), std::runtime_error);
  auto bad_tag = sealed;
  bad_tag.tag[0] ^= 1;
  EXPECT_THROW(aes_gcm_open(key, bad_tag.nonce, bad_tag.tag, bad_tag.ciphertext, aad), std::runtime_error);
  EXPECT_THROW(aes_gcm_open(key, sealed.nonce, sealed.tag, sealed.ciphertext, Bytes{8}), std::runtime_error);
}

TEST(Crypto, ConstantTimeEqualComparesLengthAndContent) {
  EXPECT_TRUE(constant_time_equal(Bytes{1, 2}, Bytes{1, 2}));
  EXPECT_FALSE(constant_time_equal(Bytes{1, 2}, Bytes{1, 3}));
  EXPECT_FALSE(constant_time_equal(Bytes{1, 2}, Bytes{1, 2, 3}));
}

TEST(NodeId, IsSixteenRandomBytesShownAsHex) {
  const auto a = NodeId::random();
  const auto b = NodeId::random();
  EXPECT_NE(a, b);
  EXPECT_EQ(to_string(a).size(), 32u);
  EXPECT_EQ(NodeId::parse(to_string(a)), a);
  EXPECT_FALSE(NodeId::parse("xyz").has_value());
  EXPECT_FALSE(NodeId::parse(std::string(30, 'a')).has_value());
}

// The cluster key file is a secret shared by every node. Its derivation is
// protocol 23's and is pinned here by its labels: master is SHA-256 of the
// file without trailing line ends; auth and the cluster id are HKDF of master
// under "macha/auth/v1" and "macha/cluster-id/v1".
TEST(ClusterKey, DerivesFromTheFileWithoutItsLineEnds) {
  test::TempDir dir;
  const auto a = dir.path() / "a.key";
  const auto b = dir.path() / "b.key";
  std::ofstream(a) << "shared secret\n\r\n";
  std::ofstream(b) << "shared secret";
  const auto ka = load_cluster_key(a);
  const auto kb = load_cluster_key(b);
  EXPECT_EQ(ka.master, kb.master);
  const std::string secret = "shared secret";
  const auto master = sha256(std::span(reinterpret_cast<const uint8_t*>(secret.data()), secret.size()));
  EXPECT_EQ(ka.master, master.bytes);
  const std::string auth_label = "macha/auth/v1";
  EXPECT_EQ(ka.auth, hkdf_sha256(ka.master, {}, std::span(reinterpret_cast<const uint8_t*>(auth_label.data()), auth_label.size())));
  const std::string id_label = "macha/cluster-id/v1";
  const auto id = hkdf_sha256(ka.master, {}, std::span(reinterpret_cast<const uint8_t*>(id_label.data()), id_label.size()));
  EXPECT_TRUE(std::equal(ka.cluster_id.begin(), ka.cluster_id.end(), id.begin()));
}

TEST(ClusterKey, AnotherSecretIsAnotherCluster) {
  test::TempDir dir;
  std::ofstream(dir.path() / "a.key") << "one";
  std::ofstream(dir.path() / "b.key") << "two";
  const auto a = load_cluster_key(dir.path() / "a.key");
  const auto b = load_cluster_key(dir.path() / "b.key");
  EXPECT_NE(a.auth, b.auth);
  EXPECT_NE(a.cluster_id, b.cluster_id);
}

TEST(ClusterKey, RefusesAMissingOrEmptyFile) {
  test::TempDir dir;
  EXPECT_THROW(load_cluster_key(dir.path() / "absent.key"), std::runtime_error);
  std::ofstream(dir.path() / "empty.key") << "\n";
  EXPECT_THROW(load_cluster_key(dir.path() / "empty.key"), std::runtime_error);
}

// A hosted system's keys are its own derivations from the master (Dagda's
// storage key is "macha/storage/v1"); Dannan names none of them.
TEST(ClusterKey, DerivesAHostedSystemsKeyUnderItsLabel) {
  test::TempDir dir;
  std::ofstream(dir.path() / "a.key") << "secret";
  const auto key = load_cluster_key(dir.path() / "a.key");
  const std::string label = "macha/storage/v1";
  EXPECT_EQ(key.derive("macha/storage/v1"), hkdf_sha256(key.master, {}, std::span(reinterpret_cast<const uint8_t*>(label.data()), label.size())));
}

// Sealing under a given nonce is what sealing under a fresh one does with
// that nonce: deterministic, and opened by the ordinary open. It exists so
// a frame can be compared byte for byte with another implementation's.
TEST(Crypto, SealingUnderAGivenNonceMatchesTheRandomPath) {
  std::array<uint8_t, 32> key{};
  for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i * 7 + 1);
  const Bytes plaintext{'f', 'r', 'a', 'm', 'e'};
  const Bytes associated{1, 2, 3, 4};
  const auto random = aes_gcm_seal(key, plaintext, associated);
  const auto given = aes_gcm_seal(key, random.nonce, plaintext, associated);
  EXPECT_EQ(given.nonce, random.nonce);
  EXPECT_EQ(given.tag, random.tag);
  EXPECT_EQ(given.ciphertext, random.ciphertext);
  EXPECT_EQ(aes_gcm_open(key, given.nonce, given.tag, given.ciphertext, associated), plaintext);
}
}  // namespace
}  // namespace dannan::identity
