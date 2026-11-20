//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A random 16-byte id (TODO/2026-11-20-stack-plan.md, item 16): Louhi's TaskId and RequestId,
// Lugh's record ids, Kvasir's transaction ids. One type per kind of thing, so two kinds of id
// never mix.

#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <type_traits>
#include <unordered_set>

#include "dannan/identity/random_id.h"

namespace dannan::identity {
namespace {

struct TaskTag {};
struct RequestTag {};
using TaskId = RandomId<TaskTag>;
using RequestId = RandomId<RequestTag>;

TEST(RandomId, TwoAreNeverTheSame) {
  std::unordered_set<TaskId, RandomIdHash<TaskTag>> seen;
  for (int i = 0; i < 1000; ++i) EXPECT_TRUE(seen.insert(TaskId::random()).second);
}

TEST(RandomId, TextIsThirtyTwoLowercaseHexDigits) {
  const auto id = TaskId::random();
  const auto text = to_string(id);
  ASSERT_EQ(text.size(), 32u);
  for (const char c : text) EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << text;
}

TEST(RandomId, ParsesItsTextInEitherCase) {
  const auto id = TaskId::random();
  auto text = to_string(id);
  EXPECT_EQ(TaskId::parse(text), id);
  for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  EXPECT_EQ(TaskId::parse(text), id);
}

TEST(RandomId, RefusesAnythingElse) {
  EXPECT_FALSE(TaskId::parse(""));
  EXPECT_FALSE(TaskId::parse("00112233445566778899aabbccddeef"));
  EXPECT_FALSE(TaskId::parse("00112233445566778899aabbccddeeff00"));
  EXPECT_FALSE(TaskId::parse("00112233445566778899aabbccddeefg"));
}

TEST(RandomId, IsOrderedByItsBytes) {
  TaskId low, high;
  high.bytes[0] = 1;
  EXPECT_LT(low, high);
}

TEST(RandomId, KindsDoNotMix) {
  static_assert(!std::is_convertible_v<TaskId, RequestId>);
  static_assert(!std::is_constructible_v<RequestId, TaskId>);
  SUCCEED();
}

}  // namespace
}  // namespace dannan::identity
