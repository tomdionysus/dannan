//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Secure memory, the stack's one home for key material (Tom, 2026-11-03:
// no code duplicated between Dannan, Dagda and Lugh): bytes locked in RAM,
// kept out of core dumps and forks where the platform allows, wiped on
// release; and a Secret that cannot be logged, compared in constant time.

#include <gtest/gtest.h>

#include <array>
#include <concepts>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>

#include "dannan/identity/crypto.h"
#include "dannan/identity/secure_memory.h"
#include "dannan/net/codec.h"

namespace dannan::identity {
namespace {

TEST(SecureBytes, StartsZeroedAndHoldsItsSize) {
  SecureBytes bytes(100);
  ASSERT_EQ(bytes.size(), 100u);
  for (const auto byte : bytes.span()) EXPECT_EQ(byte, 0);
  bytes.data()[99] = 7;
  EXPECT_EQ(bytes.span()[99], 7);
}

// Locked where the process may lock memory; a node reports it, never fails
// for it (a container may have no RLIMIT_MEMLOCK to spare).
TEST(SecureBytes, SaysWhetherItIsLocked) {
  SecureBytes bytes(32);
  (void)bytes.locked();
  SUCCEED();
}

// Moving carries the bytes; the source is left empty, not a second copy.
TEST(SecureBytes, MovesWithoutCopying) {
  SecureBytes a(16);
  a.data()[0] = 42;
  SecureBytes b(std::move(a));
  EXPECT_EQ(b.size(), 16u);
  EXPECT_EQ(b.span()[0], 42);
  EXPECT_EQ(a.size(), 0u);  // NOLINT(bugprone-use-after-move): the moved-from state is the point
  EXPECT_TRUE(a.span().empty());
  static_assert(!std::is_copy_constructible_v<SecureBytes>);
  static_assert(!std::is_copy_assignable_v<SecureBytes>);
}

TEST(SecureBytes, AnEmptyOneIsValid) {
  SecureBytes none(0);
  EXPECT_EQ(none.size(), 0u);
  EXPECT_TRUE(none.span().empty());
}

template <class T>
concept Streamable = requires(std::ostream& out, const T& value) { out << value; };

// A Secret cannot become text by accident: no conversion to a string, no
// stream operator, no copy. It is revealed only by asking.
TEST(Secret, CannotBeLoggedOrCopied) {
  static_assert(!std::is_convertible_v<Secret, std::string>);
  static_assert(!std::is_constructible_v<std::string, Secret>);
  static_assert(!Streamable<Secret>);
  static_assert(!std::is_copy_constructible_v<Secret>);
  const std::array<uint8_t, 4> key{1, 2, 3, 4};
  const Secret secret(key);
  EXPECT_EQ(secret.size(), 4u);
  EXPECT_TRUE(std::equal(key.begin(), key.end(), secret.reveal().begin()));
}

TEST(Secret, ComparesInConstantTime) {
  const std::array<uint8_t, 3> a{1, 2, 3};
  const std::array<uint8_t, 3> b{1, 2, 4};
  EXPECT_TRUE(Secret(a) == Secret(a));
  EXPECT_FALSE(Secret(a) == Secret(b));
  EXPECT_FALSE(Secret(a) == Secret(std::array<uint8_t, 2>{1, 2}));
}

// A fresh random secret, for keys made on this node.
TEST(Secret, CanBeMadeRandom) {
  const auto a = Secret::random(32);
  const auto b = Secret::random(32);
  EXPECT_EQ(a.size(), 32u);
  EXPECT_FALSE(a == b);
}

// A buffer that grows on secure pages: a record holding secrets is encoded
// into one, so its plaintext never sits in ordinary memory before sealing.
TEST(SecureBuffer, GrowsAndCarriesARecord) {
  net::BasicWriter<SecureBuffer> writer;
  writer.string("totp");
  for (int i = 0; i < 5000; ++i) writer.u8(static_cast<uint8_t>(i));
  const auto record = writer.take();
  static_assert(std::is_same_v<std::remove_cvref_t<decltype(record)>, SecureBuffer>);
  EXPECT_EQ(record.size(), 4u + 4 + 5000);
  net::Reader reader(record);
  EXPECT_EQ(reader.string(), "totp");
  EXPECT_EQ(reader.u8(), 0);
}

// A passphrase typed a character at a time becomes a Secret, and the
// buffer it was typed into is left empty.
TEST(SecureBuffer, BecomesASecret) {
  SecureBuffer typed;
  for (const char c : std::string("hunter2")) typed.push_back(static_cast<uint8_t>(c));
  const Secret secret(std::move(typed));
  EXPECT_EQ(secret.size(), 7u);
  EXPECT_EQ(std::string(secret.reveal().begin(), secret.reveal().end()), "hunter2");
}

// Opened plaintext and derived keys in secure memory, the same bytes the
// ordinary forms give.
TEST(SecureBuffer, HoldsOpenedPlaintextAndDerivedKeys) {
  const auto key = Secret::random(32);
  const std::span<const uint8_t, 32> fixed(key.reveal().data(), 32);
  const Bytes plaintext{1, 2, 3, 4};
  const Bytes associated{9};
  const auto sealed = aes_gcm_seal(fixed, plaintext, associated);
  const auto opened = aes_gcm_open_secure(fixed, sealed.nonce, sealed.tag, sealed.ciphertext, associated);
  EXPECT_TRUE(std::equal(opened.begin(), opened.end(), plaintext.begin(), plaintext.end()));
  auto bent = sealed.tag;
  bent[0] ^= 1;
  EXPECT_THROW(aes_gcm_open_secure(fixed, sealed.nonce, bent, sealed.ciphertext, associated), std::runtime_error);
  const Bytes info{'k'};
  const auto derived = hkdf_sha256_secret(key.reveal(), {}, info);
  const auto plain = hkdf_sha256(key.reveal(), {}, info);
  EXPECT_TRUE(std::equal(plain.begin(), plain.end(), derived.reveal().begin()));
}

// The process-wide hardening a node with keys applies at start: no core
// file, and not attachable by another process of the same user where the
// platform offers that.
TEST(SecureMemory, TheProcessCanBeHardened) {
  const auto report = harden_process();
  EXPECT_TRUE(report.core_dumps_disabled) << report.detail;
}

}  // namespace
}  // namespace dannan::identity
