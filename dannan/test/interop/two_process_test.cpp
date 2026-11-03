//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The two-process test: a Macha node (the binary DANNAN_MACHA_BINARY names)
// and a Dannan node (dannan-node) form one cluster over loopback. Step 2's
// gate: the handshake, the Dannan node in Macha's membership and Status,
// ping and members, Macha's telemetry received, and a DATA-lane call over a
// lane Macha dialled back, so each side has dialled the other.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <fstream>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"
#include "dannan/test/fixtures/ports.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "tools/macha_types.h"

extern char** environ;

namespace {

using namespace std::chrono_literals;

// A child process, ended with SIGTERM (then SIGKILL) when the test is done.
class Child {
 public:
  Child(const std::vector<std::string>& argv, const std::filesystem::path& log, bool pipe_stdout) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int fds[2] = {-1, -1};
    if (pipe_stdout) {
      if (::pipe(fds) != 0) throw std::runtime_error("pipe failed");
      posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
      posix_spawn_file_actions_addclose(&actions, fds[0]);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    } else {
      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    }
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    const int spawned = posix_spawn(&_pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (pipe_stdout) {
      ::close(fds[1]);
      _out = fds[0];
    }
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

bool accepts(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  ::close(fd);
  return connected;
}

// One HTTP/1.1 exchange with Connection: close; the whole response.
std::string http(uint16_t port, const std::string& method, const std::string& path, const std::string& body, const std::string& token = {}) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return {};
  }
  std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
  if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
  if (!body.empty()) request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n" + body;
  (void)::send(fd, request.data(), request.size(), 0);
  std::string response;
  char chunk[8192];
  for (;;) {
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, 5000) <= 0) break;
    const auto got = ::recv(fd, chunk, sizeof(chunk), 0);
    if (got <= 0) break;
    response.append(chunk, static_cast<size_t>(got));
  }
  ::close(fd);
  return response;
}

std::string random_key() {
  std::random_device device;
  std::string out;
  for (int i = 0; i < 64; ++i) out += "0123456789abcdef"[device() % 16];
  return out;
}

bool any_line(const std::vector<std::string>& lines, const std::string& pattern) {
  const std::regex expression(pattern);
  for (const auto& line : lines)
    if (std::regex_search(line, expression)) return true;
  return false;
}

// A Macha node from a config under the test's directory, ready (by
// /api/v1/health) when constructed.
struct MachaNode {
  dannan::test::TempDir dir;
  uint16_t port = dannan::test::free_port();
  uint16_t api = dannan::test::free_port();
  std::unique_ptr<Child> process;

  MachaNode() {
    std::filesystem::create_directories(dir.path() / "state");
    std::filesystem::create_directories(dir.path() / "cache");
    std::ofstream(dir.path() / "cluster.key") << random_key() << "\n";
    std::ofstream(dir.path() / "macha.yaml") << "state_path: " << (dir.path() / "state").string() << "\n"
                                             << "key_file: " << (dir.path() / "cluster.key").string() << "\n"
                                             << "log_level: DEBUG\n"
                                             << "storage:\n  hosts_extents: false\n"
                                             << "cache:\n  path: " << (dir.path() / "cache").string() << "\n  max_blocks: 64\n"
                                             << "network:\n  listen: 127.0.0.1\n  advertise: 127.0.0.1\n  port: " << port << "\n"
                                             << "  inbound_capable: true\n  heartbeat_ms: 1000\n  telemetry_interval_ms: 1000\n"
                                             << "  dead_after_ms: 10000\n  connect_timeout_ms: 2500\n"
                                             << "catalogue:\n  scanner:\n    enabled: false\n  api:\n    enabled: true\n"
                                             << "    listen: 127.0.0.1\n    port: " << api << "\n"
                                             << "ingest:\n  enabled: false\ntorrent:\n  enabled: false\nstreaming:\n  enabled: false\n";
    process = std::make_unique<Child>(std::vector<std::string>{DANNAN_MACHA_BINARY, "--config", (dir.path() / "macha.yaml").string()}, dir.path() / "macha.log",
                                      false);
  }
  // Ready, not merely listening: Macha binds its storage routes and answers
  // Status only once its services are up, which /api/v1/health says with a
  // 200.
  bool ready() const {
    for (int i = 0; i < 600; ++i) {
      if (accepts(api) && http(api, "GET", "/api/v1/health", {}).starts_with("HTTP/1.1 200") && accepts(port)) return true;
      std::this_thread::sleep_for(100ms);
    }
    return false;
  }
  std::string status_nodes() const {
    const auto session = http(api, "POST", "/api/v1/session", "{}");
    std::smatch token;
    if (!std::regex_search(session, token, std::regex("\"token\"\\s*:\\s*\"([^\"]+)\""))) return {};
    return http(api, "GET", "/api/v1/status/nodes", {}, token[1]);
  }
};

TEST(TwoProcess, ADannanNodeAndAMachaNodeFormOneCluster) {
  MachaNode macha;
  ASSERT_TRUE(macha.ready()) << "the Macha node did not become ready; its log is " << (macha.dir.path() / "macha.log");

  Child node({DANNAN_NODE_BINARY, "--key", (macha.dir.path() / "cluster.key").string(), "--port", "0", "--peer", "127.0.0.1:" + std::to_string(macha.port),
              "--seconds", "4"},
             macha.dir.path() / "dannan-node.log", true);
  std::vector<std::string> lines;
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  std::string self;
  std::string status;
  while (auto line = node.line(deadline)) {
    lines.push_back(*line);
    if (line->starts_with("listening ")) self = line->substr(10, 32);
    // While both run: the Dannan node in Macha's Status.
    if (*line == "script done") status = macha.status_nodes();
    if (*line == "stopped") break;
  }
  std::string all;
  for (const auto& line : lines) all += line + "\n";
  ASSERT_EQ(self.size(), 32u) << all;

  // Handshake and members: Macha's roster lists the Dannan node.
  EXPECT_TRUE(any_line(lines, "^members type 102 ")) << all;
  EXPECT_TRUE(any_line(lines, "^member " + self + " self ")) << all;
  EXPECT_TRUE(any_line(lines, "^ping type 100 dials 1 ")) << all;
  // Macha dialled the DATA lane back when asked, and answered get_object over
  // it: a real 404, and no second dial.
  EXPECT_TRUE(any_line(lines, "^admitted [0-9a-f]{32} data$")) << all;
  EXPECT_TRUE(any_line(lines, "^data lane open$")) << all;
  EXPECT_TRUE(any_line(lines, "^get_object refused 404 not_found dials 1$")) << all;
  // Macha's telemetry reached the Dannan node.
  EXPECT_TRUE(any_line(lines, "^telemetry [0-9a-f]{32} [1-9][0-9]*$")) << all;
  // And Macha's Status lists it.
  EXPECT_NE(status.find(self), std::string::npos) << status;
  EXPECT_TRUE(any_line(lines, "^stopped$")) << all;
}

// Step 5's gate: Dannan's whole node, not just its transport, joins a
// running Macha cluster as a bare member through a bootstrap seed. Each
// lists the other, Dannan's node has Macha's telemetry, and Macha's Status
// lists Dannan's node.
TEST(TwoProcess, ADannanNodeJoinsAMachaClusterAsABareMember) {
  MachaNode macha;
  ASSERT_TRUE(macha.ready()) << "the Macha node did not become ready; its log is " << (macha.dir.path() / "macha.log");
  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { io.run(); });

  dannan::NodeConfig config;
  config.state_path = macha.dir.path() / "dannan-state";
  config.key = dannan::identity::load_cluster_key(macha.dir.path() / "cluster.key");
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = 0;
  config.bootstrap = {{"127.0.0.1", macha.port}};
  config.inbound = dannan::reach::Inbound::yes;
  config.heartbeat = std::chrono::milliseconds(500);
  config.telemetry_interval = std::chrono::milliseconds(500);
  config.version = "0.1.0";
  {
    dannan::Node node(io, config);
    dannan::tools::macha_protocol23_types([&](dannan::net::MessageType type, dannan::net::Registration registration) {
      node.context().routes().add(
          type, std::move(registration),
          [](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message&) { return dannan::net::Handled::unrecognised(); });
    });
    node.start();
    const auto self = dannan::identity::to_string(node.context().identity().id());
    std::optional<dannan::identity::NodeId> macha_id;
    std::string status;
    bool telemetry = false;
    const auto until = std::chrono::steady_clock::now() + 30s;
    while (std::chrono::steady_clock::now() < until) {
      for (const auto& member : node.context().nodes().active())
        if (member.port == macha.port) macha_id = member.id;
      if (macha_id) {
        for (const auto& record : node.context().telemetry().all())
          if (record.node_id() == *macha_id) telemetry = true;
      }
      status = macha.status_nodes();
      if (macha_id && telemetry && status.find(self) != std::string::npos) break;
      std::this_thread::sleep_for(200ms);
    }
    ASSERT_TRUE(macha_id) << "Macha never joined the Dannan node's roster";
    EXPECT_TRUE(node.context().nodes().directly_reachable(*macha_id));
    EXPECT_TRUE(telemetry) << "no telemetry from Macha";
    EXPECT_NE(status.find(self), std::string::npos) << status;
    node.stop();
  }
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
}

}  // namespace
