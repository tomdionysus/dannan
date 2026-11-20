//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// SHA-1 and HMAC-SHA-1 (TODO/2026-11-20-stack-plan.md, item 12) against the published vectors:
// FIPS 180's for SHA-1, RFC 2202's for HMAC-SHA-1, and RFC 6238's first TOTP value, which is
// what Lugh computes with it.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "dannan/identity/crypto.h"

namespace dannan::identity {
namespace {

std::vector<uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

TEST(Sha1, MatchesFips180) {
  EXPECT_EQ(hex(sha1(bytes(""))), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(hex(sha1(bytes("abc"))), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(hex(sha1(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(HmacSha1, MatchesRfc2202) {
  EXPECT_EQ(hex(hmac_sha1(std::vector<uint8_t>(20, 0x0b), bytes("Hi There"))), "b617318655057264e28bc0b6fb378c8ef146be00");
  EXPECT_EQ(hex(hmac_sha1(bytes("Jefe"), bytes("what do ya want for nothing?"))), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
  // A key longer than the block is hashed first (case 6).
  EXPECT_EQ(hex(hmac_sha1(std::vector<uint8_t>(80, 0xaa), bytes("Test Using Larger Than Block-Size Key - Hash Key First"))),
            "aa4ae5e15272d00e95705637ce8a3b55ed402112");
}

TEST(HmacSha1, GivesRfc6238sFirstTotp) {
  // The SHA-1 seed, at T = 59 s with a 30 s step: counter 1, eight digits 94287082.
  const auto key = bytes("12345678901234567890");
  const std::vector<uint8_t> counter{0, 0, 0, 0, 0, 0, 0, 1};
  const auto mac = hmac_sha1(key, counter);
  const auto offset = mac[19] & 0x0f;
  const uint32_t code = ((mac[offset] & 0x7fu) << 24) | (mac[offset + 1] << 16) | (mac[offset + 2] << 8) | mac[offset + 3];
  EXPECT_EQ(code % 100'000'000u, 94287082u);
}

}  // namespace
}  // namespace dannan::identity
