//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Text forms of bytes and UTF-8 checks (TODO/2026-11-20-stack-plan.md, item 9; from Lugh's
// core/encoding.h, with Louhi's padded base64url): RFC 4648's test vectors, and exact decoding,
// so one value has one spelling.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "dannan/encoding/base.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/secure_memory.h"

namespace dannan::encoding {
namespace {

std::vector<uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }

TEST(Base64, MatchesRfc4648) {
  EXPECT_EQ(base64(bytes("")), "");
  EXPECT_EQ(base64(bytes("f")), "Zg==");
  EXPECT_EQ(base64(bytes("fo")), "Zm8=");
  EXPECT_EQ(base64(bytes("foo")), "Zm9v");
  EXPECT_EQ(base64(bytes("foob")), "Zm9vYg==");
  EXPECT_EQ(base64(bytes("fooba")), "Zm9vYmE=");
  EXPECT_EQ(base64(bytes("foobar")), "Zm9vYmFy");
}

TEST(Base64, DecodesExactlyWhatItEncodes) {
  for (const auto* text : {"", "f", "fo", "foo", "foob", "fooba", "foobar"}) EXPECT_EQ(base64_decode(base64(bytes(text))), bytes(text)) << text;
  const std::vector<uint8_t> high{0xfb, 0xff, 0xbf};
  EXPECT_EQ(base64(high), "+/+/");
  EXPECT_EQ(base64_decode("+/+/"), high);
}

TEST(Base64, RefusesAnyOtherSpelling) {
  EXPECT_FALSE(base64_decode("Zg="));     // length
  EXPECT_FALSE(base64_decode("Zg"));      // unpadded
  EXPECT_FALSE(base64_decode("Zh=="));    // trailing bits set
  EXPECT_FALSE(base64_decode("-_-_"));    // the url alphabet
  EXPECT_FALSE(base64_decode("Z==="));    // too much padding
  EXPECT_FALSE(base64_decode("Zm9v\n"));  // whitespace
  EXPECT_FALSE(base64_decode("Z=g="));    // padding inside
}

TEST(Base64Url, UnpaddedByDefault) {
  EXPECT_EQ(base64url(bytes("f")), "Zg");
  EXPECT_EQ(base64url(bytes("fo")), "Zm8");
  const std::vector<uint8_t> high{0xfb, 0xff, 0xbf};
  EXPECT_EQ(base64url(high), "-_-_");
  EXPECT_EQ(base64url_decode("-_-_"), high);
  EXPECT_EQ(base64url_decode("Zg"), bytes("f"));
}

TEST(Base64Url, PaddedWhenAsked) {
  EXPECT_EQ(base64url(bytes("f"), Padding::padded), "Zg==");
  EXPECT_EQ(base64url(bytes("fo"), Padding::padded), "Zm8=");
  EXPECT_EQ(base64url_decode("Zg==", Padding::padded), bytes("f"));
  EXPECT_FALSE(base64url_decode("Zg", Padding::padded));
  EXPECT_FALSE(base64url_decode("Zg==", Padding::none));
}

TEST(Base64Url, RefusesAnyOtherSpelling) {
  EXPECT_FALSE(base64url_decode("Z"));   // a length no encoder makes
  EXPECT_FALSE(base64url_decode("Zh"));  // trailing bits set
  EXPECT_FALSE(base64url_decode("+/"));  // the standard alphabet
}

TEST(Base32, MatchesRfc4648UpperCaseUnpadded) {
  EXPECT_EQ(base32(bytes("")), "");
  EXPECT_EQ(base32(bytes("f")), "MY");
  EXPECT_EQ(base32(bytes("fo")), "MZXQ");
  EXPECT_EQ(base32(bytes("foo")), "MZXW6");
  EXPECT_EQ(base32(bytes("foob")), "MZXW6YQ");
  EXPECT_EQ(base32(bytes("fooba")), "MZXW6YTB");
  EXPECT_EQ(base32(bytes("foobar")), "MZXW6YTBOI");
}

TEST(Base32, DecodesEitherCase) {
  EXPECT_EQ(base32_decode("MZXW6YTBOI"), bytes("foobar"));
  EXPECT_EQ(base32_decode("mzxw6ytboi"), bytes("foobar"));
}

TEST(Base32, RefusesAnyOtherSpelling) {
  EXPECT_FALSE(base32_decode("MZXW6YTBOI======"));  // padding
  EXPECT_FALSE(base32_decode("MZ"));                // trailing bits set
  EXPECT_FALSE(base32_decode("M"));                 // a length no encoder makes
  EXPECT_FALSE(base32_decode("MZ1"));               // outside the alphabet
}

TEST(Base32, DecodesASecretIntoSecureMemory) {
  const auto secret = base32_decode<identity::SecureBuffer>("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
  ASSERT_TRUE(secret);
  EXPECT_EQ(std::string(secret->begin(), secret->end()), "12345678901234567890");
}

TEST(Hex, IsLowerCaseAndDecodesEitherCase) {
  const std::vector<uint8_t> data{0x00, 0xab, 0xff};
  EXPECT_EQ(hex(data), "00abff");
  EXPECT_EQ(unhex("00ABff"), data);
  EXPECT_FALSE(unhex("0"));
  EXPECT_FALSE(unhex("zz"));
}

TEST(Hex, IdentityKeepsItsNamesForOneMinor) {
  const std::vector<uint8_t> data{0x12, 0x34};
  EXPECT_EQ(identity::hex(data), hex(data));
  EXPECT_EQ(identity::unhex("1234"), unhex("1234"));
}

TEST(Utf8, LengthsOfValidSequences) {
  EXPECT_EQ(utf8_length("a", 0), 1u);
  EXPECT_EQ(utf8_length("\xc3\xa9", 0), 2u);          // e acute
  EXPECT_EQ(utf8_length("\xe2\x82\xac", 0), 3u);      // euro sign
  EXPECT_EQ(utf8_length("\xf0\x9f\x98\x80", 0), 4u);  // U+1F600
}

TEST(Utf8, RefusesInvalidSequences) {
  EXPECT_EQ(utf8_length("\xc0\xaf", 0), 0u);          // overlong
  EXPECT_EQ(utf8_length("\xed\xa0\x80", 0), 0u);      // a surrogate
  EXPECT_EQ(utf8_length("\xf4\x90\x80\x80", 0), 0u);  // past U+10FFFF
  EXPECT_EQ(utf8_length("\xc3", 0), 0u);              // truncated
  EXPECT_EQ(utf8_length("\xc3\x28", 0), 0u);          // a bad continuation
  EXPECT_EQ(utf8_length("\x80", 0), 0u);              // a lone continuation
  EXPECT_EQ(utf8_length("\xf8\x88\x80\x80\x80", 0), 0u);
}

TEST(Utf8, ValidWholeStrings) {
  EXPECT_TRUE(valid_utf8(""));
  EXPECT_TRUE(valid_utf8("plain caf\xc3\xa9"));
  EXPECT_FALSE(valid_utf8("bad \xc3"));
}

TEST(Printable, RefusesControlCharactersIncludingC1) {
  EXPECT_TRUE(printable("Hello, caf\xc3\xa9 \xe2\x82\xac"));
  EXPECT_FALSE(printable(std::string("nul\0", 4)));
  EXPECT_FALSE(printable("tab\t"));
  EXPECT_FALSE(printable("del\x7f"));
  EXPECT_FALSE(printable("c1 \xc2\x85"));  // U+0085, NEL
  EXPECT_FALSE(printable("bad \xc3"));
  EXPECT_TRUE(printable(""));
}

}  // namespace
}  // namespace dannan::encoding
