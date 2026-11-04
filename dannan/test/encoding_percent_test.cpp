//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Percent-encoding, the stack's one copy: RFC 3986 strict decoding (a
// malformed escape is an error, '+' literal), form decoding ('+' a space),
// and encoding by a set of literal bytes that always decodes back.

#include <gtest/gtest.h>

#include <stdexcept>

#include "dannan/encoding/percent.h"

namespace dannan::encoding {
namespace {

TEST(Percent, StrictDecodingTakesOnlyEscapes) {
  EXPECT_EQ(percent_decode("a%20b"), "a b");
  EXPECT_EQ(percent_decode("a+b"), "a+b");
  EXPECT_EQ(percent_decode("%e2%82%ac %E2%82%AC"), "\u20ac \u20ac");
  EXPECT_EQ(percent_decode(""), "");
  EXPECT_EQ(percent_decode("%00", Decoding::strict), std::string(1, '\0'));
  for (const auto* malformed : {"%", "100%", "%1", "%zz", "%g0", "a%2", "%%41"}) EXPECT_THROW(percent_decode(malformed), std::invalid_argument) << malformed;
}

TEST(Percent, FormDecodingTakesPlusAsSpace) {
  EXPECT_EQ(percent_decode("a+b%2Bc", Decoding::form), "a b+c");
  EXPECT_THROW(percent_decode("a+%zz", Decoding::form), std::invalid_argument);
}

TEST(Percent, EncodingLeavesItsSetLiteralInUpperCaseHex) {
  EXPECT_EQ(percent_encode("AZaz09-._~ /?#[]@!$&'()*+,;=%"), "AZaz09-._~%20%2F%3F%23%5B%5D%40%21%24%26%27%28%29%2A%2B%2C%3B%3D%25");
  EXPECT_EQ(percent_encode("\u20ac"), "%E2%82%AC");
  EXPECT_EQ(percent_encode("a b+c*~", PercentSet::form()), "a+b%2Bc*%7E");
  EXPECT_EQ(percent_encode("a/b c", PercentSet::unreserved().keep("/")), "a/b%20c");
  // Lugh's topic-scope rule: printable ASCII but '%', '{' and '}' literal;
  // space, controls and non-ASCII encoded.
  const auto scope = PercentSet::printable_ascii().encode("{}");
  EXPECT_EQ(percent_encode("a/+/#/{x} 100%\t\u00e9", scope), "a/+/#/%7Bx%7D%20100%25%09%C3%A9");
  // '%' is never literal, whatever the set says.
  EXPECT_EQ(percent_encode("%", PercentSet::unreserved().keep("%")), "%25");
}

// Every byte, through every preset, decodes back to itself.
TEST(Percent, EveryEncodingDecodesBack) {
  std::string all;
  for (int byte = 0; byte < 256; ++byte) all += static_cast<char>(byte);
  EXPECT_EQ(percent_decode(percent_encode(all, PercentSet::unreserved())), all);
  EXPECT_EQ(percent_decode(percent_encode(all, PercentSet::printable_ascii())), all);
  EXPECT_EQ(percent_decode(percent_encode(all, PercentSet::form()), Decoding::form), all);
  EXPECT_EQ(percent_decode(percent_encode(all, PercentSet::form().keep("+")), Decoding::form), all);
}

}  // namespace
}  // namespace dannan::encoding
