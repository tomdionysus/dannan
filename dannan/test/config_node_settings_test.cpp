//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The node settings every node binary in the stack shares (0.11.0; asked by Lir, Kvasir, Louhi
// and Mimir; the maintainer ruled their section cluster.*): one list of keys, flags and variables
// by docs/config.md's rule under each project's prefix, read into a NodeConfig in the reader's
// words, so no binary keeps a copy.

#include "dannan/config/node_settings.h"

#include <gtest/gtest.h>

#include <fstream>

#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::config {
namespace {

class NoEnvironment final : public Environment {
 public:
  std::optional<std::string> get(const std::string&) const override { return std::nullopt; }
};

const Setting* setting(const Reader& reader, const std::string& key) {
  for (const auto& one : reader.settings)
    if (one.key == key) return &one;
  return nullptr;
}

TEST(NodeSettings, EachKeyIsUnderClusterWithItsFlagAndTheProjectsVariable) {
  Reader reader;
  add_node_settings(reader, "LIR");
  for (const auto* key : {"cluster.key", "cluster.bootstrap", "cluster.state", "cluster.name", "cluster.domain", "cluster.relay", "cluster.listen.address",
                          "cluster.listen.port", "cluster.listen.advertise", "cluster.listen.inbound", "cluster.heartbeat_ms", "cluster.path_class",
                          "cluster.punch_ms", "cluster.reprobe_ms", "cluster.custody.passphrase_env", "cluster.custody.credential",
                          "cluster.custody.credentials_dir", "cluster.identity.ca", "cluster.identity.certificate", "cluster.identity.revocations",
                          "cluster.identity.no_cluster_key"})
    EXPECT_NE(setting(reader, key), nullptr) << key;
  EXPECT_EQ(reader.settings.size(), 21u);
  const auto* port = setting(reader, "cluster.listen.port");
  ASSERT_NE(port, nullptr);
  EXPECT_EQ(port->flag, "--cluster-listen-port");
  EXPECT_EQ(port->variable, "LIR_CLUSTER_LISTEN_PORT");
  const auto* env = setting(reader, "cluster.custody.passphrase_env");
  ASSERT_NE(env, nullptr);
  EXPECT_EQ(env->flag, "--cluster-custody-passphrase-env");
  EXPECT_EQ(env->variable, "LIR_CLUSTER_CUSTODY_PASSPHRASE_ENV");
  EXPECT_EQ(setting(reader, "cluster.relay")->kind, Setting::Kind::toggle);
  EXPECT_EQ(setting(reader, "cluster.bootstrap")->kind, Setting::Kind::list);
}

// A host's own settings sit beside them, untouched.
TEST(NodeSettings, AHostsOwnSettingsStayBesideThem) {
  Reader reader;
  reader.settings.push_back({"mqtt.listen.port", Setting::Kind::value, "--mqtt-listen-port", "LIR_MQTT_LISTEN_PORT", "a port"});
  add_node_settings(reader, "LIR");
  NoEnvironment none;
  reader.environment = &none;
  const auto settings = reader.read({"--mqtt-listen-port", "1883", "--cluster-listen-port", "7500"});
  EXPECT_EQ(settings.number("mqtt.listen.port", 0, 65535), 1883u);
  EXPECT_EQ(node_config(settings).port, 7500);
}

TEST(NodeSettings, TheyFillANodeConfig) {
  test::TempDir dir;
  std::ofstream(dir.path() / "k") << "a long shared secret";
  Reader reader;
  add_node_settings(reader, "KVASIR");
  NoEnvironment none;
  reader.environment = &none;
  const auto settings = reader.read({"--cluster-key", (dir.path() / "k").string(), "--cluster-state", (dir.path() / "state").string(), "--cluster-bootstrap",
                                     "192.0.2.10:7437", "--cluster-bootstrap", "192.0.2.11:7438", "--cluster-name", "n1", "--cluster-domain", "rack-2",
                                     "--cluster-relay", "--cluster-listen-address", "127.0.0.1", "--cluster-listen-port", "7600", "--cluster-listen-advertise",
                                     "192.0.2.20", "--cluster-listen-inbound", "no", "--cluster-heartbeat-ms", "500", "--cluster-path-class", "punchable",
                                     "--cluster-punch-ms", "4000", "--cluster-reprobe-ms", "9000"});
  const auto config = node_config(settings);
  EXPECT_EQ(config.state_path, dir.path() / "state");
  EXPECT_EQ(config.key.auth, identity::load_cluster_key(dir.path() / "k").auth);
  ASSERT_EQ(config.bootstrap.size(), 2u);
  EXPECT_EQ(config.bootstrap[1].host, "192.0.2.11");
  EXPECT_EQ(config.bootstrap[1].port, 7438);
  EXPECT_EQ(config.node_name, "n1");
  EXPECT_EQ(config.failure_domain, "rack-2");
  EXPECT_TRUE(config.relay);
  EXPECT_EQ(config.listen_host, "127.0.0.1");
  EXPECT_EQ(config.port, 7600);
  EXPECT_EQ(config.advertise_host, "192.0.2.20");
  EXPECT_EQ(config.inbound, reach::Inbound::no);
  EXPECT_EQ(config.heartbeat, std::chrono::milliseconds(500));
  // Liveness follows the heartbeat as NodeConfig's own defaults have it: six beats.
  EXPECT_EQ(config.dead_after, std::chrono::milliseconds(3000));
  EXPECT_EQ(config.path_class, membership::PathClass::punchable);
  EXPECT_EQ(config.punch_window, std::chrono::milliseconds(4000));
  EXPECT_EQ(config.reprobe_while_incapable, std::chrono::milliseconds(9000));
}

// Nothing is required: whether a state or a key must be given is the binary's rule (Mimir runs
// a cluster of one with neither). Nothing given leaves NodeConfig's defaults.
TEST(NodeSettings, NothingGivenLeavesTheDefaults) {
  Reader reader;
  add_node_settings(reader, "MIMIR");
  NoEnvironment none;
  reader.environment = &none;
  const auto config = node_config(reader.read({}));
  const NodeConfig defaults;
  EXPECT_TRUE(config.state_path.empty());
  EXPECT_EQ(config.port, defaults.port);
  EXPECT_EQ(config.listen_host, defaults.listen_host);
  EXPECT_EQ(config.heartbeat, defaults.heartbeat);
  EXPECT_EQ(config.dead_after, defaults.dead_after);
  EXPECT_FALSE(config.custody);
  EXPECT_FALSE(config.certificate);
  EXPECT_TRUE(node_revocations(reader.read({})).empty());
}

// A value that cannot be used, and a pair given half, are refused in the reader's words.
TEST(NodeSettings, ABadValueOrAHalfPairIsRefused) {
  Reader reader;
  add_node_settings(reader, "LOUHI");
  NoEnvironment none;
  reader.environment = &none;
  try {
    (void)node_config(reader.read({"--cluster-listen-inbound", "maybe"}));
    FAIL() << "no refusal";
  } catch (const ConfigError& error) {
    EXPECT_NE(std::string(error.what()).find("--cluster-listen-inbound"), std::string::npos) << error.what();
  }
  EXPECT_THROW((void)node_config(reader.read({"--cluster-bootstrap", "no-port-here"})), ConfigError);
  EXPECT_THROW((void)node_config(reader.read({"--cluster-identity-ca", "/nowhere/ca.pem"})), ConfigError);
  EXPECT_THROW((void)node_config(reader.read({"--cluster-identity-no-cluster-key"})), ConfigError);
}

}  // namespace
}  // namespace dannan::config
