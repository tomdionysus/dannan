//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Node key custody, the early slice of step 10 (TODO/ACTIVE.md,
// "Foundations for the stack"): the node's private key and the seal root
// hosted systems derive node-local keys from, sealed on disk under a root
// that is never there in clear: a systemd credential, or a typed
// passphrase stretched with Argon2id. A wrong root and an altered file are
// refused alike; custody is never replaced.

#include <gtest/gtest.h>

#include <fstream>

#include "dannan/identity/argon2.h"
#include "dannan/identity/key_custody.h"
#include "dannan/net/codec.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::identity {
namespace {

// Light costs: the tests prove the shape, not the hardness.
const Argon2Costs light{1024, 1, 1};

Secret phrase(const std::string& text) { return Secret(std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size())); }

void put_credential(const std::filesystem::path& directory, const std::string& name, const Secret& root) {
  std::filesystem::create_directories(directory);
  std::ofstream(directory / name, std::ios::binary).write(reinterpret_cast<const char*>(root.reveal().data()), static_cast<std::streamsize>(root.size()));
}

TEST(Argon2id, IsDeterministicPerSaltAndCosts) {
  const std::array<uint8_t, 16> salt{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const auto password = phrase("correct horse");
  const auto a = argon2id(password.reveal(), salt, light);
  EXPECT_EQ(a.size(), 32u);
  EXPECT_TRUE(a == argon2id(password.reveal(), salt, light));
  auto other_salt = salt;
  other_salt[0] = 9;
  EXPECT_FALSE(a == argon2id(password.reveal(), other_salt, light));
  EXPECT_FALSE(a == argon2id(phrase("correct horsf").reveal(), salt, light));
  EXPECT_FALSE(a == argon2id(password.reveal(), salt, Argon2Costs{2048, 1, 1}));
  EXPECT_EQ(argon2id(password.reveal(), salt, light, 64).size(), 64u);
  EXPECT_THROW(argon2id(password.reveal(), std::span<const uint8_t>(salt.data(), 4), light), std::invalid_argument);
}

// RFC 9106's test vector for Argon2id, so the parameters mean what they
// say everywhere they are used.
TEST(Argon2id, MatchesRfc9106) {
  const std::vector<uint8_t> password(32, 0x01);
  const std::vector<uint8_t> salt(16, 0x02);
  const std::vector<uint8_t> secret(8, 0x03);
  const std::vector<uint8_t> associated(12, 0x04);
  const auto tag = argon2id(password, salt, Argon2Costs{32, 3, 4}, 32, secret, associated);
  EXPECT_EQ(hex(tag.reveal()), "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");
}

// Costs from a file or a record are checked against a ceiling before
// anything is spent.
TEST(Argon2id, RefusesCostsBeyondTheCeiling) {
  const std::array<uint8_t, 16> salt{};
  EXPECT_TRUE(Argon2Costs{}.valid());
  EXPECT_FALSE((Argon2Costs{Argon2Costs::max_memory_kib + 1, 1, 1}.valid()));
  EXPECT_FALSE((Argon2Costs{1024, Argon2Costs::max_iterations + 1, 1}.valid()));
  EXPECT_FALSE((Argon2Costs{1u << 20, 1, Argon2Costs::max_lanes + 1}.valid()));
  EXPECT_FALSE((Argon2Costs{1024, 0, 1}.valid()));
  EXPECT_THROW(argon2id(phrase("p").reveal(), salt, Argon2Costs{Argon2Costs::max_memory_kib + 1, 1, 1}), std::invalid_argument);
}

TEST(KeyCustody, MadeUnderACredentialOpensUnderIt) {
  test::TempDir dir;
  const auto credentials = dir.path() / "credentials";
  const auto source = KeySource::systemd_credential("dannan-node-key", credentials);
  EXPECT_FALSE(KeyCustody::exists(dir.path()));
  const auto root = KeyCustody::create(dir.path(), source);
  ASSERT_TRUE(root);
  EXPECT_EQ(root->size(), 32u);
  EXPECT_TRUE(KeyCustody::exists(dir.path()));
  put_credential(credentials, "dannan-node-key", *root);
  const auto custody = KeyCustody::open(dir.path(), source);
  EXPECT_EQ(custody.node_key().algorithm(), SigningAlgorithm::es256);
  // The same key every start.
  EXPECT_EQ(KeyCustody::open(dir.path(), source).node_key().public_key(), custody.node_key().public_key());
  const auto message = std::vector<uint8_t>{1, 2, 3};
  EXPECT_TRUE(verify(SigningAlgorithm::es256, custody.node_key().public_key(), message, custody.node_key().sign(message)));
}

// The root is never on disk in clear, and neither is anything under it.
TEST(KeyCustody, NothingIsOnDiskInClear) {
  test::TempDir dir;
  const auto source = KeySource::systemd_credential("k", dir.path() / "credentials");
  const auto root = KeyCustody::create(dir.path(), source);
  put_credential(dir.path() / "credentials", "k", *root);
  const auto custody = KeyCustody::open(dir.path(), source);
  std::ifstream input(dir.path() / "identity" / "custody", std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(bytes.substr(0, 8), "DNNKEY01");
  const auto pkcs8 = custody.node_key().to_pkcs8();
  const std::string key(reinterpret_cast<const char*>(pkcs8.reveal().data()), pkcs8.size());
  const std::string raw(reinterpret_cast<const char*>(root->reveal().data()), root->size());
  EXPECT_EQ(bytes.find(key), std::string::npos);
  EXPECT_EQ(bytes.find(raw), std::string::npos);
}

TEST(KeyCustody, AWrongRootOrAnAlteredFileIsRefused) {
  test::TempDir dir;
  const auto credentials = dir.path() / "credentials";
  const auto source = KeySource::systemd_credential("k", credentials);
  const auto root = KeyCustody::create(dir.path(), source);
  put_credential(credentials, "k", Secret::random(32));
  EXPECT_THROW(KeyCustody::open(dir.path(), source), std::runtime_error);
  put_credential(credentials, "k", *root);
  EXPECT_NO_THROW(KeyCustody::open(dir.path(), source));
  const auto path = dir.path() / "identity" / "custody";
  std::string bytes;
  {
    std::ifstream input(path, std::ios::binary);
    bytes.assign((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  }
  for (const size_t at : {size_t{12}, bytes.size() - 1}) {
    auto altered = bytes;
    altered[at] = static_cast<char>(altered[at] ^ 1);
    std::ofstream(path, std::ios::binary) << altered;
    EXPECT_THROW(KeyCustody::open(dir.path(), source), std::runtime_error) << at;
  }
}

// A missing credential, or custody that was never made, says so in the
// operator's terms (discipline 3).
TEST(KeyCustody, SaysWhatIsMissing) {
  test::TempDir dir;
  const auto source = KeySource::systemd_credential("absent", dir.path() / "credentials");
  try {
    (void)KeyCustody::open(dir.path(), source);
    FAIL();
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("no key custody"), std::string::npos) << error.what();
  }
  (void)KeyCustody::create(dir.path(), source);
  try {
    (void)KeyCustody::open(dir.path(), source);
    FAIL();
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("absent"), std::string::npos) << error.what();
  }
}

TEST(KeyCustody, IsNeverReplaced) {
  test::TempDir dir;
  const auto source = KeySource::systemd_credential("k", dir.path() / "credentials");
  (void)KeyCustody::create(dir.path(), source);
  EXPECT_THROW(KeyCustody::create(dir.path(), source), std::runtime_error);
}

// Without systemd credentials: a passphrase typed at each start,
// stretched with Argon2id under the salt and costs the file records.
TEST(KeyCustody, MadeUnderAPassphraseOpensUnderIt) {
  test::TempDir dir;
  int asked = 0;
  const auto source = KeySource::typed_passphrase(
      [&] {
        ++asked;
        return phrase("a long passphrase");
      },
      light);
  EXPECT_FALSE(KeyCustody::create(dir.path(), source));
  const auto custody = KeyCustody::open(dir.path(), source);
  EXPECT_EQ(asked, 2);
  const auto wrong = KeySource::typed_passphrase([] { return phrase("a long passphrasf"); }, light);
  EXPECT_THROW(KeyCustody::open(dir.path(), wrong), std::runtime_error);
  // Each source opens only what it made.
  EXPECT_THROW(KeyCustody::open(dir.path(), KeySource::systemd_credential("k", dir.path() / "credentials")), std::runtime_error);
  (void)custody;
}

// A crafted file asking for a terabyte is refused before the passphrase is
// asked for or a byte spent.
TEST(KeyCustody, RefusesCraftedCostsBeforeSpendingAnything) {
  test::TempDir dir;
  net::Writer header;
  for (const char c : std::string("DNNKEY01")) header.u8(static_cast<uint8_t>(c));
  header.u8(static_cast<uint8_t>(KeySource::Kind::passphrase));
  header.u32(0xffffffffu);
  header.u32(1);
  header.u32(1);
  header.fixed(std::array<uint8_t, 16>{});
  header.fixed(std::array<uint8_t, 16>{});
  header.fixed(std::array<uint8_t, 12>{});
  header.fixed(std::array<uint8_t, 16>{});
  header.bytes(net::Bytes(8, 0));
  std::filesystem::create_directories(dir.path() / "identity");
  std::ofstream(dir.path() / "identity" / "custody", std::ios::binary)
      .write(reinterpret_cast<const char*>(header.data().data()), static_cast<std::streamsize>(header.data().size()));
  bool asked = false;
  const auto source = KeySource::typed_passphrase([&] {
    asked = true;
    return phrase("p");
  });
  EXPECT_THROW(KeyCustody::open(dir.path(), source), std::runtime_error);
  EXPECT_FALSE(asked);
}

// Keys for hosted systems' node-local use (Dagda seals its node-local
// domain keys under one): one per label, the same every start, in secure
// memory, and never the node key.
TEST(KeyCustody, DerivesNodeLocalKeysByLabel) {
  test::TempDir dir;
  const auto source = KeySource::typed_passphrase([] { return phrase("p"); }, light);
  (void)KeyCustody::create(dir.path(), source);
  const auto custody = KeyCustody::open(dir.path(), source);
  const auto a = custody.derive("dagda/node-local/1");
  EXPECT_EQ(a.size(), 32u);
  EXPECT_TRUE(a == KeyCustody::open(dir.path(), source).derive("dagda/node-local/1"));
  EXPECT_FALSE(a == custody.derive("lugh/keyring/1"));
  EXPECT_THROW(custody.derive(""), std::invalid_argument);
}

}  // namespace
}  // namespace dannan::identity
