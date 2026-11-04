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
//               [CUSTODY] [--ca FILE --certificate FILE [--no-cluster-key]]
//               [--revocations FILE]
//   dannan-node --state DIR --init-custody CUSTODY
//   dannan-node --state DIR --show-identity CUSTODY
//
// Certificates (wire version 2): --init-custody makes the node's key
// custody (under a credential, printing the new root, hex, for
// `systemd-creds encrypt`); --show-identity prints its node id and public
// key as JSON, for `dannan-ca issue`; --ca and --certificate run it on the
// certificate dannan-ca issued, --no-cluster-key by certificate alone.
// CUSTODY is --custody-passphrase-env VAR or --custody-credential NAME
// [--credentials-dir DIR]. --revocations applies a dannan-ca revocation file
// at start and passes it on.
//
// Every option can also come from a YAML file (--config FILE, -c FILE, or
// $DANNAN_NODE_CONFIG), secrets.yaml beside it, or a DANNAN_NODE_*
// variable, the flags winning: `--port 7437` is `listen: {port: 7437}` and
// DANNAN_NODE_LISTEN_PORT (dannan/config/config.h; reader() below lists
// them all).

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
  std::string custody_passphrase_env;
  std::string custody_credential;
  std::string credentials_dir;
  std::string ca;
  std::string certificate;
  std::string revocations;
  bool no_cluster_key = false;
  bool init_custody = false;
  bool show_identity = false;
};

net::Endpoint endpoint(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos) throw std::runtime_error("expected HOST:PORT, got " + text);
  return {text.substr(0, colon), static_cast<uint16_t>(std::stoi(text.substr(colon + 1)))};
}

// Every setting, its flag, and its variable (DANNAN_NODE_ and the key in
// capitals), read through the stack's one reader: defaults, the file
// (--config, $DANNAN_NODE_CONFIG), secrets.yaml beside it, the
// environment, then the flags.
config::Reader reader() {
  using Kind = config::Setting::Kind;
  config::Reader out;
  const auto add = [&](std::string key, Kind kind, std::string flag, std::string what) {
    std::string variable = "DANNAN_NODE_";
    for (const char c : key) variable += c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    out.settings.push_back({std::move(key), kind, std::move(flag), std::move(variable), std::move(what)});
  };
  add("cluster.key", Kind::value, "--key", "the cluster key file");
  add("cluster.bootstrap", Kind::list, "--bootstrap", "HOST:PORT");
  add("node.state", Kind::value, "--state", "the state directory");
  add("node.name", Kind::value, "--name", "a name");
  add("node.domain", Kind::value, "--domain", "a failure domain");
  add("node.relay", Kind::toggle, "--relay", "whether it relays for others");
  add("listen.address", Kind::value, "--listen", "an address");
  add("listen.port", Kind::value, "--port", "a port");
  add("listen.advertise", Kind::value, "--advertise", "the host peers dial");
  add("listen.inbound", Kind::value, "--inbound", "yes, no or auto");
  add("timing.heartbeat_ms", Kind::value, "--heartbeat-ms", "milliseconds");
  add("timing.status_ms", Kind::value, "--status-ms", "milliseconds");
  add("timing.echo_ms", Kind::value, "--echo-ms", "milliseconds");
  add("timing.reprobe_ms", Kind::value, "--reprobe-ms", "milliseconds");
  add("timing.punch_ms", Kind::value, "--punch-ms", "milliseconds");
  add("timing.seconds", Kind::value, "--seconds", "seconds to run");
  add("reach.path_class", Kind::value, "--path-class", "public, punchable or relay_only");
  add("custody.passphrase_env", Kind::value, "--custody-passphrase-env", "a variable");
  add("custody.credential", Kind::value, "--custody-credential", "a credential name");
  add("custody.credentials_dir", Kind::value, "--credentials-dir", "a directory");
  add("identity.ca", Kind::value, "--ca", "the CA certificate, PEM");
  add("identity.certificate", Kind::value, "--certificate", "the node certificate, PEM");
  add("identity.revocations", Kind::value, "--revocations", "a revocations file");
  add("identity.no_cluster_key", Kind::toggle, "--no-cluster-key", "certificate alone");
  add("command.init_custody", Kind::toggle, "--init-custody", "make the custody and exit");
  add("command.show_identity", Kind::toggle, "--show-identity", "print the identity and exit");
  out.config_variable = "DANNAN_NODE_CONFIG";
  return out;
}

Options parse(int argc, char** argv) {
  const auto settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
  if (!settings.positional.empty()) throw std::runtime_error("unexpected argument " + settings.positional.front());
  for (const auto& warning : settings.warnings) std::cerr << "dannan-node: " << warning << "\n";
  Options out;
  const auto ms = [&](const std::string& key, std::chrono::milliseconds otherwise) {
    const auto value = settings.number(key, 0, 86400000);
    return value ? std::chrono::milliseconds(*value) : otherwise;
  };
  out.key = settings.path_of("cluster.key").value_or("");
  for (const auto& seed : settings.list("cluster.bootstrap")) out.bootstrap.push_back(endpoint(seed));
  out.state = settings.path_of("node.state").value_or("");
  out.name = settings.text("node.name").value_or("");
  out.domain = settings.text("node.domain").value_or("");
  out.relay = settings.toggle("node.relay").value_or(false);
  out.listen = settings.text("listen.address").value_or(out.listen);
  out.port = static_cast<uint16_t>(settings.number("listen.port", 0, 65535).value_or(out.port));
  out.advertise = settings.text("listen.advertise").value_or("");
  if (const auto mode = settings.choice("listen.inbound", {"yes", "no", "auto"}))
    out.inbound = *mode == "yes" ? reach::Inbound::yes : *mode == "no" ? reach::Inbound::no : reach::Inbound::automatic;
  out.heartbeat = ms("timing.heartbeat_ms", out.heartbeat);
  out.status = ms("timing.status_ms", out.status);
  out.echo = ms("timing.echo_ms", out.echo);
  out.reprobe = ms("timing.reprobe_ms", out.reprobe);
  out.punch = ms("timing.punch_ms", out.punch);
  out.seconds = static_cast<int>(settings.number("timing.seconds", 0, 31536000).value_or(0));
  if (const auto name = settings.choice("reach.path_class", {"public", "punchable", "relay_only"}))
    out.path_class = *name == "public"      ? membership::PathClass::public_path
                     : *name == "punchable" ? membership::PathClass::punchable
                                            : membership::PathClass::relay_only;
  out.custody_passphrase_env = settings.text("custody.passphrase_env").value_or("");
  out.custody_credential = settings.text("custody.credential").value_or("");
  out.credentials_dir = settings.text("custody.credentials_dir").value_or("");
  out.ca = settings.path_of("identity.ca").value_or("");
  out.certificate = settings.path_of("identity.certificate").value_or("");
  out.revocations = settings.path_of("identity.revocations").value_or("");
  out.no_cluster_key = settings.toggle("identity.no_cluster_key").value_or(false);
  out.init_custody = settings.toggle("command.init_custody").value_or(false);
  out.show_identity = settings.toggle("command.show_identity").value_or(false);
  if (out.state.empty()) throw std::runtime_error("--state is required");
  if (out.init_custody || out.show_identity) return out;
  if (out.key.empty() && !out.no_cluster_key) throw std::runtime_error("--key is required, unless the node runs by certificate alone (--no-cluster-key)");
  if (out.certificate.empty() != out.ca.empty()) throw std::runtime_error("--ca and --certificate go together");
  if (out.no_cluster_key && out.certificate.empty()) throw std::runtime_error("--no-cluster-key needs --ca and --certificate");
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

std::optional<identity::KeySource> custody_source(const Options& options) {
  if (!options.custody_passphrase_env.empty()) {
    const char* value = std::getenv(options.custody_passphrase_env.c_str());
    if (!value || !*value) throw std::runtime_error("the passphrase variable " + options.custody_passphrase_env + " is empty");
    const std::string passphrase(value);
    return identity::KeySource::typed_passphrase(
        [passphrase] { return identity::Secret(std::span(reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size())); });
  }
  if (!options.custody_credential.empty()) return identity::KeySource::systemd_credential(options.custody_credential, options.credentials_dir);
  return std::nullopt;
}

identity::Certificate read_certificate(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read " + path);
  return identity::Certificate::from_pem(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()));
}

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
      const auto source = custody_source(options);
      if (!source) throw std::runtime_error("the custody's source: --custody-passphrase-env VAR or --custody-credential NAME");
      if (options.init_custody) {
        // The root of a new credential goes to stdout and nowhere else.
        auto [custody, root] = identity::KeyCustody::create_and_open(options.state, *source);
        (void)custody;
        if (root) std::printf("%s\n", identity::hex(root->reveal()).c_str());
        return 0;
      }
      const auto custody = identity::KeyCustody::open(options.state, *source);
      std::printf("{\"id\":\"%s\",\"public_key\":\"%s\"}\n", identity::to_string(identity::load_or_create_node_id(options.state)).c_str(),
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
    NodeConfig config;
    config.state_path = options.state;
    if (!options.key.empty()) config.key = identity::load_cluster_key(options.key);
    config.custody = custody_source(options);
    if (!options.certificate.empty()) {
      config.ca_certificate = read_certificate(options.ca);
      config.certificate = read_certificate(options.certificate);
      config.use_cluster_key = !options.no_cluster_key;
    }
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
    if (!options.revocations.empty()) {
      std::ifstream input(options.revocations, std::ios::binary);
      if (!input) throw std::runtime_error("cannot read " + options.revocations);
      const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      for (const auto& revocation : identity::decode_revocations(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())))
        node.context().identity().revoke(revocation);
    }
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
