//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The layered settings reader every node binary shares (Tom, 2026-11-04:
// lifted from Lugh's): defaults, the file, secrets.yaml beside it, the
// environment, then the command line, each overriding the one before; a
// value that cannot be used refused in the same words from any layer;
// unknown keys reported, never read.

#include "dannan/config/config.h"

#include <gtest/gtest.h>

#include <fstream>

#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::config {
namespace {

class FakeEnvironment final : public Environment {
 public:
  std::map<std::string, std::string> variables;
  std::optional<std::string> get(const std::string& name) const override {
    const auto found = variables.find(name);
    if (found == variables.end()) return std::nullopt;
    return found->second;
  }
};

struct Bed {
  test::TempDir dir;
  FakeEnvironment environment;
  Reader reader;

  Bed() {
    reader.settings = {
        {"server.port", Setting::Kind::value, "--port", "NODE_PORT", "a port"},
        {"server.address", Setting::Kind::value, "--address", "NODE_ADDRESS", "an address"},
        {"cluster.bootstrap", Setting::Kind::list, "--bootstrap", "NODE_BOOTSTRAP", "HOST:PORT"},
        {"node.relay", Setting::Kind::toggle, "--relay", "NODE_RELAY", "whether it relays"},
        {"tls.key", Setting::Kind::value, "--tls-key", "", "a path"},
        {"log.level", Setting::Kind::value, "", "NODE_LOG_LEVEL", "a level"},
    };
    reader.config_variable = "NODE_CONFIG";
    reader.environment = &environment;
  }
  std::string write(const std::string& name, const std::string& text) {
    const auto path = dir.path() / name;
    std::ofstream(path) << text;
    return path.string();
  }
};

TEST(Config, EachLayerOverridesTheOneBefore) {
  Bed bed;
  const auto file = bed.write("config.yaml", "server:\n  port: 1000\n  address: 10.0.0.1\ntls:\n  key: /etc/key.pem\nlog:\n  level: info\n");
  bed.write("secrets.yaml", "server:\n  port: 2000\n");
  bed.environment.variables["NODE_PORT"] = "3000";
  bed.environment.variables["NODE_LOG_LEVEL"] = "debug";
  const auto settings = bed.reader.read({"--config", file, "--port", "4000"});
  EXPECT_EQ(settings.path, file);
  EXPECT_EQ(settings.secrets_path, (bed.dir.path() / "secrets.yaml").string());
  EXPECT_EQ(settings.number("server.port", 0, 65535), 4000u);
  EXPECT_EQ(settings.find("server.port")->source, "--port");
  EXPECT_EQ(settings.text("server.address"), "10.0.0.1");
  EXPECT_EQ(settings.find("server.address")->source, file);
  EXPECT_EQ(settings.text("log.level"), "debug");
  EXPECT_EQ(settings.find("log.level")->source, "NODE_LOG_LEVEL");
  EXPECT_EQ(settings.path_of("tls.key"), "/etc/key.pem");
  // Given by no layer: nothing, for the binary's default.
  EXPECT_FALSE(settings.text("cluster.bootstrap"));
  EXPECT_FALSE(settings.toggle("node.relay"));
  // Secrets win over the file, and the environment over secrets.
  bed.environment.variables.erase("NODE_PORT");
  EXPECT_EQ(bed.reader.read({"--config", file}).number("server.port", 0, 65535), 2000u);
}

// The file: named by the flag, else the variable (either must exist), else
// the default if it exists, else none.
TEST(Config, WhichFileIsRead) {
  Bed bed;
  const auto named = bed.write("named.yaml", "server: {port: 1}\n");
  const auto installed = bed.write("installed.yaml", "server: {port: 2}\n");
  bed.reader.default_path = installed;
  EXPECT_EQ(bed.reader.read({}).number("server.port", 0, 65535), 2u);
  bed.environment.variables["NODE_CONFIG"] = named;
  EXPECT_EQ(bed.reader.read({}).number("server.port", 0, 65535), 1u);
  EXPECT_EQ(bed.reader.read({"-c", installed}).number("server.port", 0, 65535), 2u);
  EXPECT_EQ(bed.reader.read({"--config=" + installed}).number("server.port", 0, 65535), 2u);
  EXPECT_THROW(bed.reader.read({"--config", (bed.dir.path() / "missing.yaml").string()}), ConfigError);
  bed.environment.variables["NODE_CONFIG"] = (bed.dir.path() / "missing.yaml").string();
  EXPECT_THROW(bed.reader.read({}), ConfigError);
  bed.environment.variables.clear();
  bed.reader.default_path = (bed.dir.path() / "absent.yaml").string();
  const auto none = bed.reader.read({});
  EXPECT_TRUE(none.path.empty());
  EXPECT_TRUE(none.values.empty());
}

// A key no setting reads is reported and not read; the shape of a known
// one is enforced, naming the file.
TEST(Config, UnknownKeysWarnAndShapesAreEnforced) {
  Bed bed;
  const auto file = bed.write("config.yaml", "server:\n  prot: 1\nextra: {a: 1}\n");
  const auto settings = bed.reader.read({"--config", file});
  ASSERT_EQ(settings.warnings.size(), 2u);
  // In the file's order.
  EXPECT_NE(settings.warnings[0].find("unknown key server.prot"), std::string::npos);
  EXPECT_NE(settings.warnings[1].find("unknown key extra"), std::string::npos);
  EXPECT_TRUE(settings.values.empty());
  const auto refused = [&](const std::string& text) {
    try {
      bed.reader.read({"--config", bed.write("bad.yaml", text)});
    } catch (const ConfigError& error) {
      return std::string(error.what());
    }
    return std::string();
  };
  EXPECT_NE(refused("server:\n  port: [1, 2]\n").find("server.port must be a single value"), std::string::npos);
  EXPECT_NE(refused("cluster:\n  bootstrap: a:1\n").find("cluster.bootstrap must be a list"), std::string::npos);
  EXPECT_NE(refused("server: 7\n").find("server must be a map"), std::string::npos);
  EXPECT_NE(refused("- just\n- a list\n").find("the top level must be a map"), std::string::npos);
  EXPECT_NE(refused("server: {port: [unclosed\n").find("bad.yaml"), std::string::npos);
  EXPECT_NE(refused("node:\n  relay: perhaps\n").find("node.relay is true or false"), std::string::npos);
  // An empty file is a file of defaults.
  EXPECT_TRUE(bed.reader.read({"--config", bed.write("empty.yaml", "")}).values.empty());
}

// A value that cannot be used is refused in the same words from any layer;
// only the source differs.
TEST(Config, ARefusalNamesItsSource) {
  Bed bed;
  const auto file = bed.write("config.yaml", "server:\n  port: seventy\n");
  const auto message = [&](const std::vector<std::string>& arguments) {
    try {
      (void)bed.reader.read(arguments).number("server.port", 0, 65535);
    } catch (const ConfigError& error) {
      return std::string(error.what());
    }
    return std::string();
  };
  EXPECT_EQ(message({"--config", file}), file + ": server.port is a number, 0 to 65535, not \"seventy\"");
  bed.environment.variables["NODE_PORT"] = "seventy";
  EXPECT_EQ(message({}), "NODE_PORT: server.port is a number, 0 to 65535, not \"seventy\"");
  EXPECT_EQ(message({"--port", "70000"}), "--port: server.port is a number, 0 to 65535, not \"70000\"");
  bed.environment.variables["NODE_ADDRESS"] = "here";
  EXPECT_EQ(std::string(bed.reader.read({}).refuse("server.address", "an IP address").what()), "NODE_ADDRESS: server.address is an IP address, not \"here\"");
  EXPECT_EQ(bed.reader.read({"--address", "b"}).choice("server.address", {"a", "b"}), "b");
  EXPECT_THROW((void)bed.reader.read({"--address", "c"}).choice("server.address", {"a", "b"}), ConfigError);
}

// Lists: a YAML list, a repeated flag (which replaces the layers below), a
// variable split on commas and spaces; null clears.
TEST(Config, ListsFromEveryLayer) {
  Bed bed;
  const auto file = bed.write("config.yaml", "cluster:\n  bootstrap: [a:1, b:2]\n");
  EXPECT_EQ(bed.reader.read({"--config", file}).list("cluster.bootstrap"), (std::vector<std::string>{"a:1", "b:2"}));
  bed.environment.variables["NODE_BOOTSTRAP"] = "c:3, d:4 e:5";
  EXPECT_EQ(bed.reader.read({"--config", file}).list("cluster.bootstrap"), (std::vector<std::string>{"c:3", "d:4", "e:5"}));
  EXPECT_EQ(bed.reader.read({"--config", file, "--bootstrap", "f:6", "--bootstrap=g:7"}).list("cluster.bootstrap"), (std::vector<std::string>{"f:6", "g:7"}));
  bed.environment.variables.clear();
  const auto cleared = bed.reader.read({"--config", bed.write("null.yaml", "cluster:\n  bootstrap:\n")});
  ASSERT_TRUE(cleared.find("cluster.bootstrap"));
  EXPECT_TRUE(cleared.list("cluster.bootstrap").empty());
}

// The command line: toggles take no value, an unknown flag or a missing
// value is refused, and arguments that are not flags are kept in order.
TEST(Config, TheCommandLine) {
  Bed bed;
  const auto settings = bed.reader.read({"init", "--relay", "--port", "1", "extra"});
  EXPECT_EQ(settings.toggle("node.relay"), true);
  EXPECT_EQ(settings.positional, (std::vector<std::string>{"init", "extra"}));
  EXPECT_EQ(bed.reader.read({"--relay=false"}).toggle("node.relay"), false);
  EXPECT_THROW(bed.reader.read({"--nope"}), ConfigError);
  EXPECT_THROW(bed.reader.read({"--port"}), ConfigError);
  EXPECT_THROW(bed.reader.read({"--config"}), ConfigError);
  EXPECT_THROW(bed.reader.read({"--relay=maybe"}), ConfigError);
  // A setting with no flag is not on the command line.
  EXPECT_THROW(bed.reader.read({"--log-level", "debug"}), ConfigError);
  const auto usage = bed.reader.describe();
  EXPECT_NE(usage.find("--port VALUE    server.port: a port (NODE_PORT)"), std::string::npos) << usage;
  EXPECT_NE(usage.find("--bootstrap VALUE..."), std::string::npos) << usage;
}

}  // namespace
}  // namespace dannan::config
