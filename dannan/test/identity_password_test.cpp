//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The stack's stored password (item 11; Lugh's and Kvasir's asks): Argon2id with a 16-byte salt
// and a 32-byte hash, verified in constant time, costs out of bounds refused before any work, a
// decoy for an absent name, needs_rehash, and the PHC string as its one text form.

#include <gtest/gtest.h>

#include <string>

#include "dannan/identity/password.h"

namespace dannan::identity {
namespace {

// Cheap costs, so the suite stays fast; the default's are RFC 9106's.
constexpr Argon2Costs cheap{64, 1, 1};

std::span<const uint8_t> bytes(const std::string& text) { return {reinterpret_cast<const uint8_t*>(text.data()), text.size()}; }

TEST(Password, AMadePasswordVerifiesAndAnotherDoesNot) {
  const std::string right = "correct horse", wrong = "correct horsf";
  const auto stored = make_password(bytes(right), cheap);
  EXPECT_EQ(stored.costs, cheap);
  EXPECT_TRUE(verify_password(bytes(right), stored));
  EXPECT_FALSE(verify_password(bytes(wrong), stored));
  EXPECT_FALSE(verify_password({}, stored));
}

TEST(Password, EachIsMadeUnderAFreshSalt) {
  const std::string password = "same";
  const auto a = make_password(bytes(password), cheap);
  const auto b = make_password(bytes(password), cheap);
  EXPECT_NE(a.salt, b.salt);
  EXPECT_NE(a.hash, b.hash);
  EXPECT_TRUE(verify_password(bytes(password), a));
  EXPECT_TRUE(verify_password(bytes(password), b));
}

TEST(Password, TheHashIsArgon2idOfThePasswordUnderItsSaltAndCosts) {
  const std::string password = "pa55word";
  const auto stored = make_password(bytes(password), cheap);
  const auto expected = argon2id(bytes(password), stored.salt, cheap, password_hash_bytes);
  ASSERT_EQ(expected.size(), password_hash_bytes);
  EXPECT_TRUE(std::equal(stored.hash.begin(), stored.hash.end(), expected.reveal().begin()));
}

TEST(Password, CostsOutOfBoundsAreRefusedBeforeAnyWork) {
  const std::string password = "x";
  EXPECT_THROW(make_password(bytes(password), Argon2Costs{0, 1, 1}), std::invalid_argument);
  EXPECT_THROW(make_password(bytes(password), Argon2Costs{Argon2Costs::max_memory_kib + 1, 1, 1}), std::invalid_argument);
  // A record's costs past the ceiling: false, and nothing allocated for them.
  auto stored = make_password(bytes(password), cheap);
  stored.costs.memory_kib = Argon2Costs::max_memory_kib * 2;
  EXPECT_FALSE(verify_password(bytes(password), stored));
  stored.costs = Argon2Costs{64, 0, 1};
  EXPECT_FALSE(verify_password(bytes(password), stored));
  EXPECT_FALSE(verify_decoy(bytes(password), Argon2Costs{0, 0, 0}));
}

TEST(Password, ADecoyNeverVerifies) {
  const std::string password = "anything";
  EXPECT_FALSE(verify_decoy(bytes(password), cheap));
  EXPECT_FALSE(verify_decoy({}, cheap));
}

TEST(Password, NeedsRehashWhenTheCostsDiffer) {
  const std::string password = "p";
  const auto stored = make_password(bytes(password), cheap);
  EXPECT_FALSE(needs_rehash(stored, cheap));
  EXPECT_TRUE(needs_rehash(stored, Argon2Costs{128, 1, 1}));
  EXPECT_TRUE(needs_rehash(stored, Argon2Costs{64, 2, 1}));
  EXPECT_TRUE(needs_rehash(stored, Argon2Costs{64, 1, 2}));
}

TEST(Password, ThePhcStringRoundTrips) {
  const std::string password = "round trip";
  const auto stored = make_password(bytes(password), Argon2Costs{256, 2, 2});
  const auto text = to_phc(stored);
  EXPECT_EQ(text.rfind("$argon2id$v=19$m=256,t=2,p=2$", 0), 0u) << text;
  EXPECT_EQ(text.find('='), text.find("v=19") + 1) << "no padding: " << text;
  const auto back = from_phc(text);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, stored);
  EXPECT_TRUE(verify_password(bytes(password), *back));
}

TEST(Password, ThePhcStringIsExact) {
  const auto stored = make_password(bytes(std::string("p")), cheap);
  const auto text = to_phc(stored);
  ASSERT_TRUE(from_phc(text));
  const auto replaced = [&](const std::string& from, const std::string& to) {
    auto out = text;
    out.replace(out.find(from), from.size(), to);
    return out;
  };
  EXPECT_FALSE(from_phc(replaced("argon2id", "argon2i")));
  EXPECT_FALSE(from_phc(replaced("v=19", "v=16")));
  EXPECT_FALSE(from_phc(replaced("m=64", "m=4194304")));
  EXPECT_FALSE(from_phc(replaced("t=1", "t=0")));
  EXPECT_FALSE(from_phc(replaced("p=1", "p=01")));
  EXPECT_FALSE(from_phc(replaced("m=64,t=1,p=1", "t=1,m=64,p=1")));
  EXPECT_FALSE(from_phc(text + "="));
  EXPECT_FALSE(from_phc(text + "$"));
  EXPECT_FALSE(from_phc(text.substr(0, text.size() - 1)));
  // A salt of another length: eight bytes, as the argon2 tool's examples use.
  const auto salt_start = text.find('$', text.find("p=1")) + 1;
  const auto salt_end = text.find('$', salt_start);
  auto short_salt = text;
  short_salt.replace(salt_start, salt_end - salt_start, "c29tZXNhbHQ");
  EXPECT_FALSE(from_phc(short_salt));
  EXPECT_FALSE(from_phc(""));
  EXPECT_FALSE(from_phc("$argon2id$v=19$m=64,t=1,p=1$$"));
}

}  // namespace
}  // namespace dannan::identity
