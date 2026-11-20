//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// An address range as Lugh's CIDR code has it (TODO/2026-11-20-stack-plan.md, item 9): host bits
// set past the length are refused at parse, not silently masked, so "10.0.0.1/8" in a config is
// an error the operator sees; an IPv4 address met on an IPv6 socket is the same address.

#include <gtest/gtest.h>

#include <stdexcept>

#include "dannan/listen/limits.h"

namespace dannan::listen {
namespace {

using boost::asio::ip::make_address;

TEST(Prefix, RefusesHostBitsPastTheLength) {
  EXPECT_THROW(Prefix::parse("10.0.0.1/8"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("192.168.1.1/24"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("2001:db8::1/64"), std::invalid_argument);
  EXPECT_NO_THROW(Prefix::parse("10.0.0.0/8"));
  EXPECT_NO_THROW(Prefix::parse("10.128.0.0/9"));
  EXPECT_NO_THROW(Prefix::parse("2001:db8::/64"));
  EXPECT_NO_THROW(Prefix::parse("0.0.0.0/0"));
}

TEST(Prefix, TheErrorNamesTheText) {
  try {
    (void)Prefix::parse("10.0.0.1/8");
    FAIL();
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("10.0.0.1/8"), std::string::npos);
    EXPECT_NE(std::string(e.what()).find("host bits"), std::string::npos);
  }
}

TEST(Prefix, TheLengthIsDigitsOnly) {
  EXPECT_THROW(Prefix::parse("10.0.0.0/+8"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("10.0.0.0/-8"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("10.0.0.0/8x"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("10.0.0.0/"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("10.0.0.0/33"), std::invalid_argument);
  EXPECT_THROW(Prefix::parse("2001:db8::/129"), std::invalid_argument);
}

TEST(Prefix, ABareAddressIsItsOwnRange) {
  const auto one = Prefix::parse("198.51.100.7");
  EXPECT_EQ(one.length, 32u);
  EXPECT_TRUE(one.contains(make_address("198.51.100.7")));
  EXPECT_FALSE(one.contains(make_address("198.51.100.8")));
}

TEST(Prefix, AnIpv4MappedAddressIsTheSameAddress) {
  const auto range = Prefix::parse("10.0.0.0/8");
  EXPECT_TRUE(range.contains(make_address("::ffff:10.1.2.3")));
  EXPECT_FALSE(range.contains(make_address("::ffff:11.1.2.3")));
}

TEST(Prefix, AnIpv4MappedPrefixIsReadAsIpv4) {
  const auto range = Prefix::parse("::ffff:10.0.0.0/104");
  EXPECT_TRUE(range.address.is_v4());
  EXPECT_EQ(range.length, 8u);
  EXPECT_TRUE(range.contains(make_address("10.9.9.9")));
}

TEST(Prefix, Unmapped) {
  EXPECT_EQ(unmapped(make_address("::ffff:192.0.2.1")), make_address("192.0.2.1"));
  EXPECT_EQ(unmapped(make_address("2001:db8::1")), make_address("2001:db8::1"));
  EXPECT_EQ(unmapped(make_address("192.0.2.1")), make_address("192.0.2.1"));
}

TEST(Prefix, PrintsAsItIsWritten) {
  EXPECT_EQ(Prefix::parse("10.0.0.0/8").to_string(), "10.0.0.0/8");
  EXPECT_EQ(Prefix::parse("2001:db8::/32").to_string(), "2001:db8::/32");
}

}  // namespace
}  // namespace dannan::listen
