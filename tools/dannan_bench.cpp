//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-bench: moves a large payload on the DATA lane while timing CONTROL round
// trips, and prints what it saw as one JSON line, so a number exists to compare. It
// asserts nothing and is not in the gate: a figure from one machine says little about
// another, and a timing verdict in a test is forbidden.
//
// Alone it runs two nodes in this process, over loopback. With --bootstrap and
// --peer-name it joins a cluster of dannan-node and measures against that peer's echo.
// Its nodes' state goes in a directory under the current one, and is removed.
//
//   dannan-bench [--megabytes N] [--frame BYTES] [--rounds N]
//                [--bootstrap HOST:PORT --peer-name NAME --key FILE --advertise HOST [--port N]]

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/identity/node_identity.h"
#include "dannan/node.h"

namespace {

using namespace dannan;
using namespace std::chrono_literals;
namespace asio = boost::asio;
using Clock = std::chrono::steady_clock;

// dannan-node's echoes, so a bed's nodes answer without anything added.
constexpr net::MessageType echo_control{1024};
constexpr net::MessageType echo_data{1025};
constexpr auto deadline = 600s;

uint64_t peak_rss_bytes() {
  rusage usage{};
  ::getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  return static_cast<uint64_t>(usage.ru_maxrss);
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

struct Percentiles {
  double p50 = 0;
  double p99 = 0;
  double max = 0;
  size_t samples = 0;
};

Percentiles of(std::vector<double> values) {
  Percentiles out;
  out.samples = values.size();
  if (values.empty()) return out;
  std::sort(values.begin(), values.end());
  out.p50 = values[values.size() / 2];
  out.p99 = values[std::min(values.size() - 1, values.size() * 99 / 100)];
  out.max = values.back();
  return out;
}

double ping_ms(Context& context, const membership::NodeRecord& peer) {
  const auto started = Clock::now();
  const auto outcome = context.routes().call_and_wait(peer, echo_control, {}, sched::FrameType::control, deadline);
  if (!outcome.reply) throw std::runtime_error("a ping failed: " + outcome.failure);
  return std::chrono::duration<double, std::milli>(Clock::now() - started).count();
}

void registered(Node& node) {
  const net::RequestHandler answer = [](const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
    return net::Handled::processed({net::message::ok, request.payload, {}});
  };
  node.context().routes().add(echo_control, {"echo_control", net::Lanes::control, net::Frames::any, false}, answer);
  node.context().routes().add(echo_data, {"echo_data", net::Lanes::data, net::Frames::not_control, false}, answer);
}

// What the run measured, against `peer`.
void measure(Context& context, const membership::NodeRecord& peer, uint64_t megabytes, int rounds, size_t frame, const std::string& mode) {
  std::vector<double> idle;
  for (int i = 0; i < 200; ++i) idle.push_back(ping_ms(context, peer));
  const net::Bytes payload(static_cast<size_t>(megabytes) * 1024 * 1024, 0x5a);
  std::atomic_bool moving{true};
  std::vector<double> loaded;
  std::thread pinger([&] {
    while (moving) {
      loaded.push_back(ping_ms(context, peer));
      std::this_thread::sleep_for(10ms);
    }
  });
  const auto started = Clock::now();
  std::string failure;
  for (int round = 0; round < rounds && failure.empty(); ++round) {
    const auto outcome = context.routes().call_and_wait(peer, echo_data, payload, sched::FrameType::foreground, deadline);
    if (!outcome.reply)
      failure = outcome.failure;
    else if (outcome.reply->payload.size() != payload.size())
      failure = "the echo came back the wrong size";
  }
  const auto seconds = std::chrono::duration<double>(Clock::now() - started).count();
  moving = false;
  pinger.join();
  if (!failure.empty()) throw std::runtime_error("the transfer failed: " + failure);
  // Each round crosses the lane twice: the request and its echo.
  const double mb_per_s = static_cast<double>(megabytes) * 2 * rounds / seconds;
  const auto small_started = Clock::now();
  constexpr int small_calls = 5000;
  for (int i = 0; i < small_calls; ++i) (void)ping_ms(context, peer);
  const double calls_per_s = small_calls / std::chrono::duration<double>(Clock::now() - small_started).count();
  const auto quiet = of(std::move(idle));
  const auto busy = of(std::move(loaded));
  std::printf(
      "{\"mode\":\"%s\",\"megabytes\":%llu,\"rounds\":%d,\"frame\":%zu,\"seconds\":%.3f,\"mb_per_s\":%.1f,"
      "\"ping_idle_ms\":{\"p50\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"samples\":%zu},"
      "\"ping_loaded_ms\":{\"p50\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"samples\":%zu},"
      "\"small_calls_per_s\":%.0f,\"peak_rss_bytes\":%llu}\n",
      mode.c_str(), static_cast<unsigned long long>(megabytes), rounds, frame, seconds, mb_per_s, quiet.p50, quiet.p99, quiet.max, quiet.samples, busy.p50,
      busy.p99, busy.max, busy.samples, calls_per_s, static_cast<unsigned long long>(peak_rss_bytes()));
}

NodeConfig base(const std::filesystem::path& state, const identity::ClusterKey& key, size_t frame) {
  NodeConfig config;
  config.state_path = state;
  config.key = key;
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = 0;
  config.inbound = reach::Inbound::yes;
  config.max_frame = frame;
  config.version = "bench";
  config.log = status::console_sink(status::LogFormat::text, status::LogLevel::warn);
  config.observation_interval = 0ms;
  return config;
}

std::optional<membership::NodeRecord> find_peer(Node& node, const std::string& name) {
  for (const auto& record : node.context().telemetry().all()) {
    if (record.text(status::field::node_name) != name) continue;
    for (const auto& peer : node.context().nodes().active())
      if (peer.id == record.node_id()) return peer;
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t megabytes = 32;
  int rounds = 3;
  size_t frame = 256 * 1024;
  std::string bootstrap, peer_name, key_file, advertise;
  uint16_t port = 7438;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) throw std::invalid_argument(flag + " needs a value");
      return argv[++i];
    };
    try {
      if (flag == "--megabytes")
        megabytes = std::stoull(value());
      else if (flag == "--frame")
        frame = std::stoull(value());
      else if (flag == "--rounds")
        rounds = std::stoi(value());
      else if (flag == "--bootstrap")
        bootstrap = value();
      else if (flag == "--peer-name")
        peer_name = value();
      else if (flag == "--key")
        key_file = value();
      else if (flag == "--advertise")
        advertise = value();
      else if (flag == "--port")
        port = static_cast<uint16_t>(std::stoi(value()));
      else
        throw std::invalid_argument("unknown flag " + flag);
    } catch (const std::exception& error) {
      std::cerr << error.what() << "\n";
      return 2;
    }
  }
  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < std::max(4U, std::thread::hardware_concurrency()); ++i) threads.emplace_back([&] { io.run(); });
  const auto finish = [&](int code) {
    work.reset();
    io.stop();
    for (auto& thread : threads) thread.join();
    return code;
  };
  const auto root = std::filesystem::current_path() / (".dannan-bench-" + std::to_string(::getpid()));
  try {
    std::filesystem::create_directories(root);
    if (!bootstrap.empty()) {
      const auto colon = bootstrap.rfind(':');
      auto config = base(root / "a", identity::load_cluster_key(key_file), frame);
      config.listen_host = "0.0.0.0";
      config.advertise_host = advertise;
      config.port = port;
      config.inbound = reach::Inbound::no;
      config.heartbeat = 500ms;
      config.telemetry_interval = 1000ms;
      config.node_name = "bench";
      config.bootstrap.push_back({bootstrap.substr(0, colon), static_cast<uint16_t>(std::stoi(bootstrap.substr(colon + 1)))});
      Node node(io, config);
      registered(node);
      node.start();
      std::optional<membership::NodeRecord> peer;
      for (int i = 0; i < 1200 && !peer; ++i) {
        peer = find_peer(node, peer_name);
        if (!peer) std::this_thread::sleep_for(100ms);
      }
      if (!peer) throw std::runtime_error("no active peer named " + peer_name);
      measure(node.context(), *peer, megabytes, rounds, frame, "peer:" + peer_name);
      node.stop();
    } else {
      std::ofstream(root / "cluster.key") << "bench";
      const auto key = identity::load_cluster_key(root / "cluster.key");
      Node a(io, base(root / "a", key, frame));
      registered(a);
      a.start();
      auto b_config = base(root / "b", key, frame);
      b_config.bootstrap.push_back({"127.0.0.1", a.port()});
      Node b(io, b_config);
      registered(b);
      b.start();
      const auto a_id = a.context().identity().id();
      std::optional<membership::NodeRecord> peer;
      for (int i = 0; i < 1200 && !peer; ++i) {
        for (const auto& node : b.context().nodes().active())
          if (node.id == a_id) peer = node;
        if (!peer) std::this_thread::sleep_for(50ms);
      }
      if (!peer) throw std::runtime_error("the two nodes did not meet");
      measure(b.context(), *peer, megabytes, rounds, frame, "loopback");
      b.stop();
      a.stop();
    }
    std::filesystem::remove_all(root);
    return finish(0);
  } catch (const std::exception& error) {
    std::cerr << "failed: " << error.what() << "\n";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return finish(1);
  }
}
