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

}  // namespace
}  // namespace dannan
