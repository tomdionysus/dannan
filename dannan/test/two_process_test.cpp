//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The two-process test: dannan-node (the binary DANNAN_NODE_BINARY names)
// and a node in this process form one cluster over loopback on wire v1.
// Each lists the other directly, each has the other's telemetry, and each
// calls the other's echo on both lanes, so each side has dialled the other
// and served a call over its own lanes.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"
#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan {
namespace {

using namespace std::chrono_literals;

constexpr net::MessageType echo_control{1024};
constexpr net::MessageType echo_data{1025};

struct Io {
  boost::asio::io_context context;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{context.get_executor()};
  std::vector<std::thread> threads;
  Io() {
    for (int i = 0; i < 4; ++i) threads.emplace_back([this] { context.run(); });
  }
  ~Io() {
    work.reset();
    context.stop();
    for (auto& thread : threads) thread.join();
  }
};

net::Handled answer(const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
  return net::Handled::processed({net::message::ok, request.payload, {}});
}

TEST(TwoProcess, ANodeProcessAndANodeHereFormOneCluster) {
  test::TempDir dir;
  std::ofstream(dir.path() / "cluster.key") << "two process";
  test::ChildProcess child({DANNAN_NODE_BINARY,
                            "--key",
                            (dir.path() / "cluster.key").string(),
                            "--state",
                            (dir.path() / "child").string(),
                            "--listen",
                            "127.0.0.1",
                            "--advertise",
                            "127.0.0.1",
                            "--port",
                            "0",
                            "--inbound",
                            "yes",
                            "--heartbeat-ms",
                            "200",
                            "--status-ms",
                            "200",
                            "--echo-ms",
                            "200",
                            "--seconds",
                            "60"},
                           dir.path() / "child.log");
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  std::smatch match;
  std::string child_id;
  uint16_t child_port = 0;
  while (auto line = child.line(deadline)) {
    if (std::regex_search(*line, match, std::regex(R"re("event":"listening","id":"([0-9a-f]{32})","port":([0-9]+))re"))) {
      child_id = match[1];
      child_port = static_cast<uint16_t>(std::stoi(match[2]));
      break;
    }
  }
  ASSERT_EQ(child_id.size(), 32u) << "dannan-node did not start; its log is " << (dir.path() / "child.log");

  Io io;
  NodeConfig config;
  config.state_path = dir.path() / "here";
  config.key = identity::load_cluster_key(dir.path() / "cluster.key");
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = 0;
  config.bootstrap = {{"127.0.0.1", child_port}};
  config.inbound = reach::Inbound::yes;
  config.heartbeat = 200ms;
  config.telemetry_interval = 200ms;
  config.version = "two-process";
  Node node(io.context, config);
  node.context().routes().add(echo_control, {"echo_control", net::Lanes::control, net::Frames::any, false}, answer);
  node.context().routes().add(echo_data, {"echo_data", net::Lanes::data, net::Frames::not_control, false}, answer);
  node.start();
  const auto self = identity::to_string(node.context().identity().id());
  const auto them = identity::NodeId::parse(child_id);
  ASSERT_TRUE(them);

  // The child lists this node and its echoes to it succeed on both lanes.
  bool listed_with_echoes = false;
  std::string last;
  while (auto line = child.line(deadline)) {
    last = *line;
    if (line->find("\"event\":\"status\"") == std::string::npos || line->find(self) == std::string::npos) continue;
    if (line->find("\"control\":\"ok\"") != std::string::npos && line->find("\"data\":\"ok\"") != std::string::npos) {
      listed_with_echoes = true;
      break;
    }
  }
  EXPECT_TRUE(listed_with_echoes) << last;

  // This node lists the child directly, has its telemetry, and calls its
  // echo on both lanes.
  EXPECT_TRUE(node.context().nodes().directly_reachable(*them));
  const auto roster = node.context().nodes().active();
  const auto peer = std::find_if(roster.begin(), roster.end(), [&](const membership::NodeRecord& n) { return n.id == *them; });
  ASSERT_NE(peer, roster.end());
  EXPECT_EQ(peer->port, child_port);
  bool telemetry = false;
  for (int i = 0; i < 100 && !telemetry; ++i) {
    for (const auto& record : node.context().telemetry().all())
      if (record.node_id() == *them && record.text(status::field::version) == "dannan-node") telemetry = true;
    if (!telemetry) std::this_thread::sleep_for(100ms);
  }
  EXPECT_TRUE(telemetry);
  for (const auto& [type, frame] : {std::pair{echo_control, sched::FrameType::control}, std::pair{echo_data, sched::FrameType::foreground}}) {
    const auto outcome = node.context().routes().call_and_wait(*peer, type, net::Bytes{1, 2, 3}, frame, 5s);
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->payload, (net::Bytes{1, 2, 3}));
    EXPECT_EQ(outcome.peer.id, *them);
  }
  node.stop();
}

// The operator's flow for certificates (TODO/2026-11-04-identity.md,
// section 4), by the binaries alone: dannan-ca makes a CA; each node makes
// its custody and shows its identity; the CA certifies each; the two meet
// on certificates alone and answer each other on both lanes. A revocation
// is written and reads back.
// dannan-node takes its settings from a file, secrets.yaml beside it and
// the environment as well as flags, through the stack's one reader: the
// file's port 1 (privileged, so binding it would fail) is overridden by
// DANNAN_NODE_LISTEN_PORT=0, and the key's path comes from secrets.yaml.
// A value that cannot be used stops it, naming the file.
TEST(TwoProcess, TheNodeReadsAConfigurationFile) {
  test::TempDir dir;
  std::ofstream(dir.path() / "cluster.key") << "a two-process cluster key";
  const auto config = (dir.path() / "node.yaml").string();
  std::ofstream(config) << "node:\n  state: " << (dir.path() / "state").string()
                        << "\nlisten:\n  address: 127.0.0.1\n  advertise: 127.0.0.1\n  port: 1\n  inbound: \"yes\"\ntiming:\n  seconds: 30\n";
  std::ofstream(dir.path() / "secrets.yaml") << "cluster:\n  key: " << (dir.path() / "cluster.key").string() << "\n";
  test::ChildOptions options;
  options.environment = std::vector<std::string>{"DANNAN_NODE_LISTEN_PORT=0"};
  {
    test::ChildProcess node({DANNAN_NODE_BINARY, "--config", config}, dir.path() / "node.log", options);
    bool listening = false;
    const auto until = std::chrono::steady_clock::now() + 30s;
    while (auto line = node.line(until)) {
      std::smatch match;
      if (std::regex_search(*line, match, std::regex(R"re("event":"listening","id":"[0-9a-f]{32}","port":([0-9]+))re"))) {
        listening = std::stoi(match[1]) != 1;
        break;
      }
    }
    EXPECT_TRUE(listening) << "see " << (dir.path() / "node.log");
  }
  std::ofstream(config) << "node:\n  state: " << (dir.path() / "state").string() << "\nlisten:\n  port: seventy\n";
  // Without the variable, which would win over the file's bad port.
  test::ChildOptions plain;
  plain.environment = std::vector<std::string>{};
  test::ChildProcess refused({DANNAN_NODE_BINARY, "--config", config}, dir.path() / "refused.log", plain);
  EXPECT_EQ(refused.wait(std::chrono::steady_clock::now() + 30s), 2);
  std::ifstream log(dir.path() / "refused.log");
  const std::string said((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
  EXPECT_NE(said.find(config + ": listen.port is a number, 0 to 65535, not \"seventy\""), std::string::npos) << said;
}

TEST(TwoProcess, AnOperatorCertifiesTwoNodesThatMeetOnCertificatesAlone) {
  test::TempDir dir;
  const auto deadline = [] { return std::chrono::steady_clock::now() + 30s; };
  const std::vector<std::string> env{"CA_PASS=the ca passphrase", "NODE_PASS=a node passphrase"};
  const auto run = [&](std::vector<std::string> argv, const std::string& log) {
    test::ChildOptions options;
    options.environment = env;
    test::ChildProcess child(argv, dir.path() / log, options);
    auto out = child.read_all(deadline());
    return std::make_pair(child.wait(deadline()), out);
  };
  const auto ca_dir = (dir.path() / "ca").string();
  ASSERT_EQ(run({DANNAN_CA_BINARY, "init", "--dir", ca_dir, "--name", "two process", "--passphrase-env", "CA_PASS"}, "init.log").first, 0);
  const auto shown_ca = run({DANNAN_CA_BINARY, "show", ca_dir + "/ca.pem"}, "show-ca.log");
  EXPECT_NE(shown_ca.second.find("a CA"), std::string::npos) << shown_ca.second;

  std::vector<std::string> certificates;
  std::vector<std::string> ids;
  for (const auto* name : {"one", "two"}) {
    const auto state = (dir.path() / name).string();
    ASSERT_EQ(run({DANNAN_NODE_BINARY, "--state", state, "--init-custody", "--custody-passphrase-env", "NODE_PASS"}, std::string(name) + "-init.log").first, 0);
    const auto [shown, identity_json] =
        run({DANNAN_NODE_BINARY, "--state", state, "--show-identity", "--custody-passphrase-env", "NODE_PASS"}, std::string(name) + "-show.log");
    ASSERT_EQ(shown, 0);
    std::smatch match;
    ASSERT_TRUE(std::regex_search(identity_json, match, std::regex(R"re("id":"([0-9a-f]{32})","public_key":"([0-9a-f]+)")re"))) << identity_json;
    ids.push_back(match[1]);
    const auto [issued, pem] = run({DANNAN_CA_BINARY, "issue", "--dir", ca_dir, "--passphrase-env", "CA_PASS", "--node", match[1], "--public-key", match[2]},
                                   std::string(name) + "-issue.log");
    ASSERT_EQ(issued, 0);
    const auto path = (dir.path() / (std::string(name) + ".pem")).string();
    std::ofstream(path) << pem;
    certificates.push_back(path);
  }

  test::ChildOptions running;
  running.environment = env;
  const auto node_args = [&](size_t which, const std::string& extra_port, std::vector<std::string> more) {
    std::vector<std::string> argv{DANNAN_NODE_BINARY,
                                  "--state",
                                  (dir.path() / (which == 0 ? "one" : "two")).string(),
                                  "--listen",
                                  "127.0.0.1",
                                  "--advertise",
                                  "127.0.0.1",
                                  "--port",
                                  extra_port,
                                  "--inbound",
                                  "yes",
                                  "--heartbeat-ms",
                                  "200",
                                  "--status-ms",
                                  "200",
                                  "--echo-ms",
                                  "200",
                                  "--seconds",
                                  "60",
                                  "--custody-passphrase-env",
                                  "NODE_PASS",
                                  "--ca",
                                  ca_dir + "/ca.pem",
                                  "--certificate",
                                  certificates[which],
                                  "--no-cluster-key"};
    argv.insert(argv.end(), more.begin(), more.end());
    return argv;
  };
  test::ChildProcess one(node_args(0, "0", {}), dir.path() / "one.log", running);
  uint16_t port = 0;
  const auto until = deadline();
  while (auto line = one.line(until)) {
    std::smatch match;
    if (std::regex_search(*line, match, std::regex(R"re("event":"listening","id":"[0-9a-f]{32}","port":([0-9]+))re"))) {
      port = static_cast<uint16_t>(std::stoi(match[1]));
      break;
    }
  }
  ASSERT_NE(port, 0) << "node one did not start; its log is " << (dir.path() / "one.log");
  test::ChildProcess two(node_args(1, "0", {"--bootstrap", "127.0.0.1:" + std::to_string(port)}), dir.path() / "two.log", running);
  bool met = false;
  std::string last;
  const auto meeting = deadline();
  while (auto line = two.line(meeting)) {
    last = *line;
    if (line->find(ids[0]) != std::string::npos && line->find("\"control\":\"ok\"") != std::string::npos &&
        line->find("\"data\":\"ok\"") != std::string::npos) {
      met = true;
      break;
    }
  }
  EXPECT_TRUE(met) << last;

  const auto revocation = (dir.path() / "two.revoked").string();
  ASSERT_EQ(run({DANNAN_CA_BINARY, "revoke", "--dir", ca_dir, "--passphrase-env", "CA_PASS", "--node", ids[1], "--reason", "retired", "--out", revocation},
                "revoke.log")
                .first,
            0);
  const auto shown = run({DANNAN_CA_BINARY, "show", revocation}, "show-revocation.log");
  EXPECT_NE(shown.second.find(ids[1]), std::string::npos) << shown.second;
  EXPECT_NE(shown.second.find("every certificate"), std::string::npos) << shown.second;
}

}  // namespace
}  // namespace dannan
