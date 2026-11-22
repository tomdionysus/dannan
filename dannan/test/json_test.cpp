//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// JSON through Boost.JSON, compiled once in libdannan (item 24's JSON half, in 0.11.0 for Lir's
// system topics): what Lir builds, objects of strings, int64 and uint64 kept apart, booleans and
// arrays, serialised to bytes; and a parse that never throws for bad input.

#include "dannan/json/json.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace dannan::json {
namespace {

std::string text_of(const std::vector<uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }

TEST(Json, AnObjectOfTheKindsLirUsesSerialisesCompactlyInItsOrder) {
  object topic;
  topic["client"] = "sensor-7";
  topic["connected"] = true;
  topic["sessions"] = std::int64_t{-3};
  topic["bytes"] = std::uint64_t{42};
  topic["tags"] = array{"a", "b"};
  EXPECT_EQ(text_of(to_bytes(topic)), R"({"client":"sensor-7","connected":true,"sessions":-3,"bytes":42,"tags":["a","b"]})");
}

// A count above INT64_MAX stays a uint64, and a negative stays an int64, through bytes and back.
TEST(Json, Int64AndUint64StayApartThroughARoundTrip) {
  object counts;
  counts["big"] = std::numeric_limits<std::uint64_t>::max();
  counts["small"] = std::numeric_limits<std::int64_t>::min();
  const auto bytes = to_bytes(counts);
  const auto back = parse(std::span<const uint8_t>(bytes));
  ASSERT_TRUE(back.has_value());
  const auto& again = back->as_object();
  EXPECT_EQ(again.at("big").kind(), kind::uint64);
  EXPECT_EQ(again.at("big").as_uint64(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(again.at("small").kind(), kind::int64);
  EXPECT_EQ(again.at("small").as_int64(), std::numeric_limits<std::int64_t>::min());
}

TEST(Json, AParseOfBadTextSaysWhyAndThrowsNothing) {
  std::string why;
  EXPECT_FALSE(parse(std::string_view(R"({"a":)"), &why).has_value());
  EXPECT_FALSE(why.empty());
  why.clear();
  // One text only: anything after it is refused.
  EXPECT_FALSE(parse(std::string_view(R"({"a":1} 2)"), &why).has_value());
  EXPECT_FALSE(why.empty());
  EXPECT_FALSE(parse(std::string_view("")).has_value());
}

TEST(Json, AParseReadsBytesAsText) {
  const std::string text = R"(["x",true,null,1.5])";
  const auto parsed = parse(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
  ASSERT_TRUE(parsed.has_value());
  const auto& items = parsed->as_array();
  ASSERT_EQ(items.size(), 4u);
  EXPECT_EQ(items[0].as_string(), "x");
  EXPECT_TRUE(items[1].as_bool());
  EXPECT_TRUE(items[2].is_null());
  EXPECT_DOUBLE_EQ(items[3].as_double(), 1.5);
}

}  // namespace
}  // namespace dannan::json
