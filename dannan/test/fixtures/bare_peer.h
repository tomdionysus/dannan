//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/node.h"

namespace dannan::test {

// A peer that a hosted system's test meets over the real transport: Dannan's
// node with nothing hosted on it (P6, for Macha, Lugh and Lir). Its id, place,
// adverts, telemetry and answers are the test's, so a test that needs a peer of
// some shape has one, learnt of the way any other is, by meeting it. It runs
// its own io threads. Not thread-safe to build or start; its node is.
class BarePeer {
 public:
  struct Options {
    // Whom it dials first; it is dialled too, so either way they meet.
    std::vector<net::Endpoint> bootstrap;
    // The id it runs under, written to its state before it starts; random when none.
    std::optional<identity::NodeId> id;
    std::string failure_domain;
    std::string node_name;
    // Its adverts from the start (Nodes::advertise); advertise() changes them later.
    std::map<std::string, net::Bytes> adverts;
    // Its record's fields beyond Dannan's own, written at every refresh, and their layout.
    std::function<void(status::Telemetry&)> telemetry;
    std::vector<uint16_t> telemetry_layout;
    std::chrono::milliseconds telemetry_interval{250};
    std::chrono::milliseconds heartbeat{100};
    std::chrono::milliseconds dead_after{2000};
    std::string version = "bare-peer";
    // Anything else of its configuration, before it is built.
    std::function<void(NodeConfig&)> tune;
    // Before it starts: its routes (Routes::add with a handler; a type with
    // none is answered 501).
    std::function<void(Node&)> before_start;
    size_t io_threads = 2;
  };

  // Its state in `state`, in the cluster whose key is `key`, listening on a
  // loopback port the kernel picks.
  BarePeer(const std::filesystem::path& state, identity::ClusterKey key) : BarePeer(state, std::move(key), Options{}) {}
  BarePeer(const std::filesystem::path& state, identity::ClusterKey key, Options options) {
    std::filesystem::create_directories(state);
    if (options.id) std::ofstream(state / "node-id") << to_string(*options.id) << "\n";
    NodeConfig config;
    config.state_path = state;
    config.key = std::move(key);
    config.listen_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.port = 0;
    config.inbound = reach::Inbound::yes;
    config.bootstrap = std::move(options.bootstrap);
    config.failure_domain = std::move(options.failure_domain);
    config.node_name = std::move(options.node_name);
    config.telemetry_fields = std::move(options.telemetry);
    config.telemetry_layout = std::move(options.telemetry_layout);
    config.telemetry_interval = options.telemetry_interval;
    config.heartbeat = options.heartbeat;
    config.dead_after = options.dead_after;
    config.version = std::move(options.version);
    if (options.tune) options.tune(config);
    _node = std::make_unique<Node>(_io, std::move(config));
    for (auto& [key_name, value] : options.adverts) _node->context().nodes().advertise(key_name, std::move(value));
    if (options.before_start) options.before_start(*_node);
    for (size_t i = 0; i < options.io_threads; ++i) _threads.emplace_back([this] { _io.run(); });
  }

  ~BarePeer() {
    stop();
    // Destroyed while its io threads still run: a node's stop waits on work it
    // posts to them.
    _node.reset();
    _work.reset();
    _io.stop();
    for (auto& thread : _threads) thread.join();
  }
  BarePeer(const BarePeer&) = delete;
  BarePeer& operator=(const BarePeer&) = delete;

  void start() {
    _node->start();
    _started = true;
  }
  // Stops its node, as a peer that goes away; not started again.
  void stop() {
    if (_started && !_stopped) _node->stop();
    _stopped = true;
  }

  void advertise(const std::string& key, net::Bytes value) { _node->context().nodes().advertise(key, std::move(value)); }
  void withdraw(const std::string& key) { _node->context().nodes().withdraw(key); }

  identity::NodeId id() const { return _node->context().identity().id(); }
  // Where it listens; known once started.
  net::Endpoint endpoint() const { return {"127.0.0.1", _node->port()}; }
  Node& node() noexcept { return *_node; }

 private:
  boost::asio::io_context _io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work{_io.get_executor()};
  std::vector<std::thread> _threads;
  std::unique_ptr<Node> _node;
  bool _started = false;
  bool _stopped = false;
};

}  // namespace dannan::test
