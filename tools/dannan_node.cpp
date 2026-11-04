//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-node: a Dannan node with nothing above it but an echo, for the
// two-process test and the compose test bed. It hosts two user types,
// echo on the CONTROL lane (1024) and echo on the DATA lane (1025), each
// answering with its payload, and reports what it sees as one JSON object
// per line on stdout:
//
//   {"event":"listening","id":"...","port":N}
//   {"event":"status","self":{...},"peers":[{...}]}    every --status-ms
//   {"event":"stopped"}
//
// With --echo-ms, it calls echo on every active peer on both lanes each
// interval and reports each result in the next status line. It runs for
// --seconds (0: until SIGTERM or SIGINT).
//
//   dannan-node --key FILE --state DIR [--port N] [--listen ADDR]
//               [--advertise HOST] [--bootstrap HOST:PORT]...
//               [--inbound yes|no|auto] [--relay] [--name NAME]
//               [--domain NAME] [--heartbeat-ms N] [--status-ms N]
//               [--echo-ms N] [--reprobe-ms N] [--punch-ms N]
//               [--path-class public|punchable|relay_only] [--seconds N]

#include <signal.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"

namespace {

using namespace dannan;
using namespace std::chrono_literals;
namespace asio = boost::asio;

constexpr net::MessageType echo_control{1024};
constexpr net::MessageType echo_data{1025};

std::atomic<bool> stopping{false};
std::mutex output;

void say(const std::string& line) {
  std::lock_guard lock(output);
  std::cout << line << std::endl;
}

std::string quoted(const std::string& text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
      out += escaped;
    } else {
      out += c;
    }
  }
  return out + "\"";
}

struct Options {
  std::string key;
  std::string state;
  std::string listen = "0.0.0.0";
  std::string advertise;
  uint16_t port = 7437;
  std::vector<net::Endpoint> bootstrap;
  reach::Inbound inbound = reach::Inbound::automatic;
  bool relay = false;
  std::string name;
  std::string domain;
  std::chrono::milliseconds heartbeat{1000};
  std::chrono::milliseconds status{1000};
  std::chrono::milliseconds echo{0};
  std::chrono::milliseconds reprobe{0};
  std::chrono::milliseconds punch{0};
  std::optional<membership::PathClass> path_class;
  int seconds = 0;
};

net::Endpoint endpoint(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("expected HOST:PORT, got " + text);
  return {text.substr(0, colon), static_cast<uint16_t>(std::stoi(text.substr(colon + 1)))};
}

Options parse(int argc, char** argv) {
  Options out;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    auto value = [&]() -> std::string {
      if (++i >= argc) throw std::runtime_error(option + " needs a value");
      return argv[i];
    };
    if (option == "--key")
      out.key = value();
    else if (option == "--state")
      out.state = value();
    else if (option == "--listen")
      out.listen = value();
    else if (option == "--advertise")
      out.advertise = value();
    else if (option == "--port")
      out.port = static_cast<uint16_t>(std::stoi(value()));
    else if (option == "--bootstrap")
      out.bootstrap.push_back(endpoint(value()));
    else if (option == "--inbound") {
      const auto mode = value();
      if (mode == "yes")
        out.inbound = reach::Inbound::yes;
      else if (mode == "no")
        out.inbound = reach::Inbound::no;
      else if (mode == "auto")
        out.inbound = reach::Inbound::automatic;
      else
        throw std::runtime_error("--inbound is yes, no or auto");
    } else if (option == "--relay")
      out.relay = true;
    else if (option == "--name")
      out.name = value();
    else if (option == "--domain")
      out.domain = value();
    else if (option == "--heartbeat-ms")
      out.heartbeat = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--status-ms")
      out.status = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--echo-ms")
      out.echo = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--reprobe-ms")
      out.reprobe = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--punch-ms")
      out.punch = std::chrono::milliseconds(std::stoi(value()));
    else if (option == "--path-class") {
      const auto name = value();
      if (name == "public")
        out.path_class = membership::PathClass::public_path;
      else if (name == "punchable")
        out.path_class = membership::PathClass::punchable;
      else if (name == "relay_only")
        out.path_class = membership::PathClass::relay_only;
      else
        throw std::runtime_error("--path-class is public, punchable or relay_only");
    } else if (option == "--seconds")
      out.seconds = std::stoi(value());
    else
      throw std::runtime_error("unknown option " + option);
  }
  if (out.key.empty() || out.state.empty()) throw std::runtime_error("--key and --state are required");
  return out;
}

// Each peer's last echo, by lane.
struct EchoResults {
  std::mutex mutex;
  std::map<identity::NodeId, std::map<std::string, std::string>> by_peer;
};

void echo_everyone(Node& node, EchoResults& results) {
  for (const auto& peer : node.context().nodes().active()) {
    if (peer.id == node.context().identity().id()) continue;
    for (const auto data : {false, true}) {
      const std::string lane = data ? "data" : "control";
      auto payload = net::Bytes{'e', 'c', 'h', 'o'};
      const auto outcome = node.context().routes().call_and_wait(peer, data ? echo_data : echo_control, std::move(payload),
                                                                 data ? sched::FrameType::foreground : sched::FrameType::control, 5s);
      std::string verdict;
      if (!outcome.reply)
        verdict = "failed: " + outcome.failure;
      else if (const auto refusal = net::refusal_of(*outcome.reply))
        verdict = "refused: " + refusal->message;
      else
        verdict = outcome.reply->payload == net::Bytes{'e', 'c', 'h', 'o'} ? "ok" : "wrong payload";
      std::lock_guard lock(results.mutex);
      results.by_peer[peer.id][lane] = verdict;
    }
  }
}

std::string record_json(const membership::NodeRecord& record) {
  std::ostringstream out;
  out << "{\"id\":" << quoted(identity::to_string(record.id)) << ",\"host\":" << quoted(record.host) << ",\"port\":" << record.port
      << ",\"domain\":" << quoted(record.failure_domain) << ",\"inbound\":" << (record.inbound_capable() ? "true" : "false")
      << ",\"relay\":" << (record.relays() ? "true" : "false") << ",\"path_class\":" << quoted(membership::path_class_name(record.path_class))
      << ",\"candidates\":[";
  for (size_t i = 0; i < record.candidates.size(); ++i) {
    const auto& candidate = record.candidates[i];
    out << (i ? "," : "") << "{\"host\":" << quoted(candidate.host) << ",\"port\":" << candidate.port
        << ",\"confirmed\":" << (candidate.confirmed ? "true" : "false") << "}";
  }
  out << "]}";
  return out.str();
}

std::string status_json(Node& node, EchoResults& results) {
  const auto self = node.context().nodes().self();
  std::ostringstream out;
  out << "{\"event\":\"status\",\"self\":" << record_json(self) << ",\"peers\":[";
  bool first = true;
  std::lock_guard lock(results.mutex);
  for (const auto& peer : node.context().nodes().active()) {
    if (peer.id == self.id) continue;
    out << (first ? "" : ",") << "{\"record\":" << record_json(peer)
        << ",\"direct\":" << (node.context().nodes().directly_reachable(peer.id) ? "true" : "false");
    if (const auto latency = node.context().nodes().latency(peer.id)) out << ",\"latency_ms\":" << latency->count();
    out << ",\"path\":{";
    for (const auto lane : {net::Lane::control, net::Lane::data}) {
      const auto path = node.context().nodes().path(peer.id, lane);
      out << (lane == net::Lane::control ? "" : ",") << quoted(net::lane_name(lane)) << ":" << quoted(path ? net::purpose_name(*path) : "none");
    }
    out << "}";
    out << ",\"echo\":{";
    if (const auto found = results.by_peer.find(peer.id); found != results.by_peer.end()) {
      bool first_lane = true;
      for (const auto& [lane, verdict] : found->second) {
        out << (first_lane ? "" : ",") << quoted(lane) << ":" << quoted(verdict);
        first_lane = false;
      }
    }
    out << "}}";
    first = false;
  }
  out << "]";
  const auto paths = node.diagnostics().paths;
  out << ",\"level0\":{\"punches_tried\":" << paths.punches_tried << ",\"punches_made\":" << paths.punches_made << ",\"relays_tried\":" << paths.relays_tried
      << ",\"relays_made\":" << paths.relays_made << ",\"punches_brokered\":" << paths.punches_brokered << ",\"circuits_relayed\":" << paths.circuits_relayed
      << ",\"relayed_bytes\":" << paths.relayed_bytes << ",\"circuits\":" << paths.circuits.size() << "}}";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  try {
    options = parse(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "dannan-node: " << error.what() << "\n";
    return 2;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, [](int) { stopping = true; });
  signal(SIGINT, [](int) { stopping = true; });

  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { io.run(); });

  int status = 0;
  try {
    NodeConfig config;
    config.state_path = options.state;
    config.key = identity::load_cluster_key(options.key);
    config.listen_host = options.listen;
    config.advertise_host = options.advertise;
    config.port = options.port;
    config.bootstrap = options.bootstrap;
    config.inbound = options.inbound;
    config.relay = options.relay;
    config.node_name = options.name;
    config.failure_domain = options.domain;
    config.heartbeat = options.heartbeat;
    config.dead_after = options.heartbeat * 6;
    config.telemetry_interval = options.heartbeat;
    config.version = "dannan-node";
    if (options.reprobe.count() > 0) config.reprobe_while_incapable = options.reprobe;
    if (options.punch.count() > 0) config.punch_window = options.punch;
    config.path_class = options.path_class;
    Node node(io, config);
    const auto answer = [](const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
      return net::Handled::processed({net::message::ok, request.payload, {}});
    };
    node.context().routes().add(echo_control, {"echo_control", net::Lanes::control, net::Frames::any, false}, answer);
    node.context().routes().add(echo_data, {"echo_data", net::Lanes::data, net::Frames::not_control, false}, answer);
    node.start();
    say("{\"event\":\"listening\",\"id\":" + quoted(identity::to_string(node.context().identity().id())) + ",\"port\":" + std::to_string(node.port()) + "}");

    EchoResults results;
    std::jthread echoes;
    if (options.echo.count() > 0) {
      echoes = std::jthread([&](std::stop_token stop) {
        while (!stop.stop_requested() && !stopping) {
          echo_everyone(node, results);
          for (auto waited = 0ms; waited < options.echo && !stop.stop_requested() && !stopping; waited += 50ms) std::this_thread::sleep_for(50ms);
        }
      });
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(options.seconds);
    auto next_status = std::chrono::steady_clock::now();
    while (!stopping && (options.seconds == 0 || std::chrono::steady_clock::now() < until)) {
      if (std::chrono::steady_clock::now() >= next_status) {
        say(status_json(node, results));
        next_status += options.status;
      }
      std::this_thread::sleep_for(50ms);
    }
    if (echoes.joinable()) {
      echoes.request_stop();
      echoes.join();
    }
    node.stop();
  } catch (const std::exception& error) {
    std::cerr << "dannan-node: " << error.what() << "\n";
    status = 1;
  }
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  say("{\"event\":\"stopped\"}");
  return status;
}
