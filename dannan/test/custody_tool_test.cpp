//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-custody (the binary DANNAN_CUSTODY_BINARY names), as Macha's deploy
// runs it between a node's stop and its start: custody made under a systemd
// credential, its root printed once for sealing, then opened with that root
// as the unit will find it, and neither while a node holds the state.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

#include "dannan/identity/crypto.h"
#include "dannan/identity/key_custody.h"
#include "dannan/identity/node_identity.h"
#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan {
namespace {

using namespace std::chrono_literals;

struct Ran {
  int status = -1;
  std::string out;
  std::string err;
};

Ran run(const test::TempDir& dir, std::vector<std::string> argv, std::optional<std::vector<std::string>> environment = std::nullopt) {
  argv.insert(argv.begin(), DANNAN_CUSTODY_BINARY);
  const auto log = dir.path() / "custody.log";
  test::ChildOptions options;
  options.environment = std::move(environment);
  Ran ran;
  {
    test::ChildProcess child(argv, log, options);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    ran.out = child.read_all(deadline);
    ran.status = child.wait(deadline).value_or(-1);
  }
  std::ifstream input(log);
  ran.err.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return ran;
}

// The root as systemd puts it: the file named for the credential.
void place_credential(const std::filesystem::path& directory, const std::string& name, const std::string& root) {
  std::filesystem::create_directories(directory);
  std::ofstream(directory / name) << root;
}

// init prints the new root and nothing else; show opens custody with it and
// names the node and its key; a second init is refused and changes nothing.
TEST(CustodyTool, MakesCustodyUnderACredentialAndShowsTheIdentity) {
  test::TempDir dir;
  const auto state = (dir.path() / "state").string();
  const auto made = run(dir, {"init", "--state", state, "--credential", "dannan-root"});
  ASSERT_EQ(made.status, 0) << made.err;
  ASSERT_TRUE(std::regex_match(made.out, std::regex("[0-9a-f]{64}\n"))) << made.out;
  EXPECT_TRUE(identity::KeyCustody::exists(state));
  // The node's id comes with its custody.
  EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(state) / "node-id"));

  const auto credentials = dir.path() / "credentials";
  place_credential(credentials, "dannan-root", made.out);
  const auto shown = run(dir, {"show", "--state", state, "--credential", "dannan-root", "--credentials-dir", credentials.string()});
  ASSERT_EQ(shown.status, 0) << shown.err;
  std::smatch match;
  ASSERT_TRUE(std::regex_match(shown.out, match, std::regex(R"re(\{"id":"([0-9a-f]{32})","public_key":"([0-9a-f]+)"\}\n)re"))) << shown.out;
  EXPECT_EQ(match[1].str(), identity::to_string(identity::load_or_create_node_id(state)));
  const auto opened = identity::KeyCustody::open(state, identity::KeySource::systemd_credential("dannan-root", credentials));
  EXPECT_EQ(match[2].str(), identity::hex(opened.node_key().public_key()));

  const auto again = run(dir, {"init", "--state", state, "--credential", "dannan-root"});
  EXPECT_EQ(again.status, 1);
  EXPECT_TRUE(again.out.empty()) << again.out;
  EXPECT_NE(again.err.find("never replaced"), std::string::npos) << again.err;
  const auto still = run(dir, {"show", "--state", state, "--credential", "dannan-root", "--credentials-dir", credentials.string()});
  EXPECT_EQ(still.out, shown.out);
}

// init keeps an id already there (a Macha node's, written before the cutover).
TEST(CustodyTool, InitKeepsTheNodesIdAlreadyThere) {
  test::TempDir dir;
  const auto state = dir.path() / "state";
  const auto id = identity::load_or_create_node_id(state);
  const auto made = run(dir, {"init", "--state", state.string(), "--credential", "dannan-root"});
  ASSERT_EQ(made.status, 0) << made.err;
  EXPECT_EQ(identity::load_or_create_node_id(state), id);
}

// show reads and never writes (Macha's review): a directory with no node id,
// mistyped or fresh, is refused and gains none.
TEST(CustodyTool, ShowRefusesAndWritesNothingWithoutANodeId) {
  test::TempDir dir;
  const auto state = dir.path() / "state";
  const auto made = run(dir, {"init", "--state", state.string(), "--credential", "dannan-root"});
  ASSERT_EQ(made.status, 0) << made.err;
  std::filesystem::remove(state / "node-id");
  const auto credentials = dir.path() / "credentials";
  place_credential(credentials, "dannan-root", made.out);
  const auto shown = run(dir, {"show", "--state", state.string(), "--credential", "dannan-root", "--credentials-dir", credentials.string()});
  EXPECT_EQ(shown.status, 1);
  EXPECT_TRUE(shown.out.empty()) << shown.out;
  EXPECT_NE(shown.err.find("no node identity in"), std::string::npos) << shown.err;
  EXPECT_FALSE(std::filesystem::exists(state / "node-id"));

  const auto elsewhere = dir.path() / "mistyped";
  EXPECT_EQ(run(dir, {"show", "--state", elsewhere.string(), "--credential", "dannan-root", "--credentials-dir", credentials.string()}).status, 1);
  EXPECT_FALSE(std::filesystem::exists(elsewhere));
}

// Without --credentials-dir, show reads $CREDENTIALS_DIRECTORY, as the unit
// will; a root that does not open custody is refused.
TEST(CustodyTool, ShowReadsTheUnitsCredentialsAndRefusesAWrongRoot) {
  test::TempDir dir;
  const auto state = (dir.path() / "state").string();
  const auto made = run(dir, {"init", "--state", state, "--credential", "dannan-root"});
  ASSERT_EQ(made.status, 0) << made.err;
  const auto credentials = dir.path() / "credentials";
  place_credential(credentials, "dannan-root", made.out);
  const auto shown =
      run(dir, {"show", "--state", state, "--credential", "dannan-root"}, std::vector<std::string>{"CREDENTIALS_DIRECTORY=" + credentials.string()});
  EXPECT_EQ(shown.status, 0) << shown.err;
  EXPECT_NE(shown.out.find("\"public_key\""), std::string::npos) << shown.out;

  const auto wrong = dir.path() / "wrong";
  place_credential(wrong, "dannan-root", std::string(64, '0') + "\n");
  const auto refused = run(dir, {"show", "--state", state, "--credential", "dannan-root", "--credentials-dir", wrong.string()});
  EXPECT_EQ(refused.status, 1);
  EXPECT_TRUE(refused.out.empty()) << refused.out;
  EXPECT_FALSE(refused.err.empty());
}

// Neither runs beside a node: the state's lock held refuses both, saying the
// path is in use, and init makes nothing.
TEST(CustodyTool, RefusesWhileTheStateIsLocked) {
  test::TempDir dir;
  const auto state = dir.path() / "state";
  (void)identity::load_or_create_node_id(state);
  {
    const identity::StateLock held(state);
    const auto made = run(dir, {"init", "--state", state.string(), "--credential", "dannan-root"});
    EXPECT_EQ(made.status, 1);
    EXPECT_TRUE(made.out.empty()) << made.out;
    EXPECT_NE(made.err.find("in use"), std::string::npos) << made.err;
    EXPECT_FALSE(identity::KeyCustody::exists(state));
    const auto shown = run(dir, {"show", "--state", state.string(), "--credential", "dannan-root", "--credentials-dir", dir.path().string()});
    EXPECT_EQ(shown.status, 1);
    EXPECT_NE(shown.err.find("in use"), std::string::npos) << shown.err;
  }
  EXPECT_EQ(run(dir, {"init", "--state", state.string(), "--credential", "dannan-root"}).status, 0);
}

// A usage error is 2 and changes nothing.
TEST(CustodyTool, AUsageErrorIsTwo) {
  test::TempDir dir;
  const auto state = (dir.path() / "state").string();
  EXPECT_EQ(run(dir, {}).status, 2);
  EXPECT_EQ(run(dir, {"make", "--state", state, "--credential", "x"}).status, 2);
  EXPECT_EQ(run(dir, {"init", "--state", state}).status, 2);
  EXPECT_EQ(run(dir, {"init", "--credential", "x"}).status, 2);
  EXPECT_EQ(run(dir, {"init", "--state", state, "--credential", "x", "--passphrase-env", "P"}).status, 2);
  EXPECT_EQ(run(dir, {"init", "--state", state, "--credential", "x", "--credentials-dir", state}).status, 2);
  EXPECT_FALSE(identity::KeyCustody::exists(state));
}

}  // namespace
}  // namespace dannan
