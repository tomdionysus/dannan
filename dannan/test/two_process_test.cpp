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

#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <fstream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"
#include "dannan/test/fixtures/temp_dir.h"

extern char** environ;

namespace dannan {
namespace {

using namespace std::chrono_literals;

constexpr net::MessageType echo_control{1024};
constexpr net::MessageType echo_data{1025};

// A child process, its stdout piped to the test, ended with SIGTERM (then
// SIGKILL) when the test is done.
class Child {
 public:
  Child(const std::vector<std::string>& argv, const std::filesystem::path& log) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) throw std::runtime_error("pipe failed");
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    const int spawned = posix_spawn(&_pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    _out = fds[0];
    if (spawned != 0) throw std::runtime_error("could not start " + argv[0]);
  }
  ~Child() {
    if (_out >= 0) ::close(_out);
    if (_pid <= 0) return;
    ::kill(_pid, SIGTERM);
    for (int i = 0; i < 200; ++i) {
      if (::waitpid(_pid, nullptr, WNOHANG) == _pid) return;
      std::this_thread::sleep_for(50ms);
    }
    ::kill(_pid, SIGKILL);
    ::waitpid(_pid, nullptr, 0);
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  // The next line the child writes, or nothing by the deadline.
  std::optional<std::string> line(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      if (const auto end = _buffer.find('\n'); end != std::string::npos) {
        auto out = _buffer.substr(0, end);
        _buffer.erase(0, end + 1);
        return out;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0) return std::nullopt;
      pollfd ready{_out, POLLIN, 0};
      if (::poll(&ready, 1, static_cast<int>(left)) <= 0) return std::nullopt;
      char chunk[4096];
      const auto got = ::read(_out, chunk, sizeof(chunk));
      if (got <= 0) return std::nullopt;
      _buffer.append(chunk, static_cast<size_t>(got));
    }
  }

 private:
  pid_t _pid = -1;
  int _out = -1;
  std::string _buffer;
};

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
  Child child({DANNAN_NODE_BINARY,
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
