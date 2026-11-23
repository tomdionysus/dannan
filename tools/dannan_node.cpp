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
//   dannan-node --cluster-key FILE --cluster-state DIR [--cluster-listen-port N]
//               [--cluster-listen-address ADDR] [--cluster-listen-advertise HOST]
//               [--cluster-bootstrap HOST:PORT]... [--cluster-listen-inbound yes|no|auto]
//               [--cluster-relay] [--cluster-name NAME] [--cluster-domain NAME]
//               [--cluster-heartbeat-ms N] [--cluster-reprobe-ms N] [--cluster-punch-ms N]
//               [--cluster-path-class public|punchable|relay_only] [--status-ms N]
//               [--echo-ms N] [--seconds N] [CUSTODY]
//               [--cluster-identity-ca FILE --cluster-identity-certificate FILE
//                [--cluster-identity-no-cluster-key]] [--cluster-identity-revocations FILE]
//   dannan-node --cluster-state DIR --init-custody CUSTODY
//   dannan-node --cluster-state DIR --show-identity CUSTODY
//
// Certificates (wire version 2): --init-custody makes the node's key custody (under a
// credential, printing the new root, hex, for `systemd-creds encrypt`); --show-identity
// prints its node id and public key as JSON, for `dannan-ca issue`; the identity settings run
// it on the certificate dannan-ca issued, no_cluster_key by certificate alone. CUSTODY is
// --cluster-custody-passphrase-env VAR or --cluster-custody-credential NAME
// [--cluster-custody-credentials-dir DIR]. The revocations file is applied at start and
// passed on.
//
// The cluster.* settings are every node binary's (dannan/config/node_settings.h). Every one can
// also come from a YAML file (--config FILE, -c FILE, or $DANNAN_CONFIG), secrets.yaml beside
// it, or a DANNAN_ variable, the flags winning: `--cluster-listen-port 7437` is
// `cluster: {listen: {port: 7437}}` and DANNAN_CLUSTER_LISTEN_PORT (docs/config.md).

#include <signal.h>

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/config/node_settings.h"
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

// dannan-node's own settings, beside the node settings every node binary shares
// (dannan/config/node_settings.h, all under cluster.*).
struct Options {
  config::Settings settings;
  NodeConfig node;
  std::chrono::milliseconds status{1000};
  std::chrono::milliseconds echo{0};
  int seconds = 0;
  bool init_custody = false;
  bool show_identity = false;
};

// Every setting, its flag and its variable by docs/config.md's rule (DANNAN_ and the key in
// capitals), read through the stack's one reader: defaults, the file (--config,
// $DANNAN_CONFIG), secrets.yaml beside it, the environment, then the flags.
config::Reader reader() {
  using Kind = config::Setting::Kind;
  config::Reader out;
  config::add_node_settings(out, "DANNAN");
  const auto add = [&](std::string key, Kind kind, std::string what) {
    std::string flag = "--";
    std::string variable = "DANNAN_";
    for (const char c : key) {
      flag += c == '_' ? '-' : c;
      variable += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    out.settings.push_back({std::move(key), kind, std::move(flag), std::move(variable), std::move(what)});
  };
  add("status_ms", Kind::value, "milliseconds between status lines");
  add("echo_ms", Kind::value, "milliseconds between echoes to every peer");
  add("seconds", Kind::value, "seconds to run");
  add("init_custody", Kind::toggle, "make the custody and exit");
  add("show_identity", Kind::toggle, "print the identity and exit");
  out.config_variable = "DANNAN_CONFIG";
  return out;
}

Options parse(int argc, char** argv) {
  Options out;
  out.settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
  const auto& settings = out.settings;
  if (!settings.positional.empty()) throw std::runtime_error("unexpected argument " + settings.positional.front());
  for (const auto& warning : settings.warnings) std::cerr << "dannan-node: " << warning << "\n";
  out.node = config::node_config(settings);
  if (const auto status = settings.number("status_ms", 1, 86400000)) out.status = std::chrono::milliseconds(*status);
  if (const auto echo = settings.number("echo_ms", 0, 86400000)) out.echo = std::chrono::milliseconds(*echo);
  out.seconds = static_cast<int>(settings.number("seconds", 0, 31536000).value_or(0));
  out.init_custody = settings.toggle("init_custody").value_or(false);
  out.show_identity = settings.toggle("show_identity").value_or(false);
  // dannan-node's own rules: a state always; a key unless it runs by certificate alone.
  if (out.node.state_path.empty()) throw std::runtime_error("--cluster-state is required");
  if (out.init_custody || out.show_identity) {
    if (!out.node.custody) throw std::runtime_error("the custody's source: --cluster-custody-passphrase-env VAR or --cluster-custody-credential NAME");
    return out;
  }
  if (!settings.find("cluster.key") && !(out.node.certificate && !out.node.use_cluster_key))
    throw std::runtime_error("--cluster-key is required, unless the node runs by certificate alone (--cluster-identity-no-cluster-key)");
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
  if (options.init_custody || options.show_identity) {
    try {
      const auto& state = options.node.state_path;
      if (options.init_custody) {
        // The root of a new credential goes to stdout and nowhere else.
        auto [custody, root] = identity::KeyCustody::create_and_open(state, *options.node.custody);
        (void)custody;
        if (root) std::printf("%s\n", identity::hex(root->reveal()).c_str());
        return 0;
      }
      const auto custody = identity::KeyCustody::open(state, *options.node.custody);
      std::printf("{\"id\":\"%s\",\"public_key\":\"%s\"}\n", identity::to_string(identity::load_or_create_node_id(state)).c_str(),
                  identity::hex(custody.node_key().public_key()).c_str());
      return 0;
    } catch (const std::exception& error) {
      std::cerr << "dannan-node: " << error.what() << "\n";
      return 1;
    }
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
    auto config = options.node;
    // A test bed's node: telemetry as often as it beats.
    config.telemetry_interval = config.heartbeat;
    config.version = "dannan-node";
    Node node(io, config);
    const auto answer = [](const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
      return net::Handled::processed({net::message::ok, request.payload, {}});
    };
    node.context().routes().add(echo_control, {"echo_control", net::Lanes::control, net::Frames::any, false}, answer);
    node.context().routes().add(echo_data, {"echo_data", net::Lanes::data, net::Frames::not_control, false}, answer);
    node.start();
    for (const auto& revocation : config::node_revocations(options.settings)) node.context().identity().revoke(revocation);
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
