//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Founding a cluster of three (docs/guide/founding.md): three nodes on loopback, the three of them
// the genesis set; each serves a space's acceptors from memory; one node founds the space, a name
// is changed through agreement, and another node reads it back. Exits 0 when all of it happened.
//
//   founding_three [STATE_DIR]

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "dannan/agree/memory_store.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/node_identity.h"
#include "dannan/node.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Waits, a short sleep at a time, until `done` or the bound: an example's patience, not a test's.
bool wait_until(const std::function<bool()>& done, std::chrono::seconds bound = 30s) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::sleep_for(20ms);
  }
  return true;
}

// The node's settings: its own state, the cluster's key, loopback, a quick heartbeat.
dannan::NodeConfig settings(const fs::path& state, const dannan::identity::ClusterKey& key, std::vector<dannan::net::Endpoint> seeds,
                            std::vector<dannan::identity::NodeId> genesis) {
  dannan::NodeConfig config;
  config.state_path = state;
  config.key = key;
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = 0;  // the system's choice; Node::port() says which
  config.inbound = dannan::reach::Inbound::yes;
  config.bootstrap = std::move(seeds);
  config.heartbeat = 200ms;
  config.agreement_genesis = std::move(genesis);
  config.version = "founding-example";
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dannan-example-founding";
  fs::remove_all(root);
  fs::create_directories(root);

  // The io context every node runs on, on four threads.
  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&io] { io.run(); });

  // One cluster key, made for this run.
  std::ofstream(root / "cluster.key") << "an example cluster's key, made for this run";
  const auto key = dannan::identity::load_cluster_key(root / "cluster.key");

  // The genesis set is every node's id, known before any starts: each id is made with its state.
  std::vector<dannan::identity::NodeId> genesis;
  for (int i = 0; i < 3; ++i) genesis.push_back(dannan::identity::load_or_create_node_id(root / ("node" + std::to_string(i))));

  // The first node starts alone; the others take it as their seed.
  std::vector<std::unique_ptr<dannan::Node>> nodes;
  for (int i = 0; i < 3; ++i) {
    std::vector<dannan::net::Endpoint> seeds;
    if (i > 0) seeds.push_back({"127.0.0.1", nodes[0]->port()});
    nodes.push_back(std::make_unique<dannan::Node>(io, settings(root / ("node" + std::to_string(i)), key, seeds, genesis)));
    auto& context = nodes.back()->context();
    // Each node serves the space's acceptors, here from memory, and says so with an advert, so the
    // others count it towards a majority.
    // Ready at once: it holds nothing yet, so it has lost nothing a restart would make it vote on.
    context.agreement().serve("example", "example", std::make_shared<dannan::agree::MemoryStore>(io.get_executor(), true));
    nodes.back()->start();
    context.nodes().advertise("example", {});
  }

  int status = 1;
  const auto everyone_serves = [&] {
    for (const auto& node : nodes) {
      size_t serving = 0;
      for (const auto& record : node->context().nodes().active())
        if (record.adverts.contains("example")) ++serving;
      if (serving < 3) return false;
    }
    return true;
  };
  if (!wait_until(everyone_serves)) {
    std::fprintf(stderr, "the three nodes did not find each other\n");
  } else {
    // Founding: every slot of the space recorded as served by the genesis set, once.
    std::promise<dannan::agree::Outcome> founded;
    nodes[0]->context().agreement().found("example", 10s, io.get_executor(), [&](dannan::agree::Outcome out) { founded.set_value(std::move(out)); });
    const auto founding = founded.get_future().get();
    std::printf("founded: %s\n", founding.committed ? "yes" : founding.why.c_str());

    // A change to one name, on node 1: its slot, served by the genesis set at version 0.
    const dannan::agree::Name name{'g', 'r', 'e', 'e', 't', 'i', 'n', 'g'};
    const dannan::agree::Bytes value{'h', 'e', 'l', 'l', 'o'};
    const auto slot = dannan::agree::slot_of(name, nodes[1]->context().agreement().table().slots);
    std::promise<dannan::agree::Outcome> written;
    nodes[1]->context().agreement().change(
        "example", slot, {name}, genesis, 0,
        [&](std::span<const std::optional<dannan::agree::Bytes>> current) {
          // The new value for each name; a change may read `current` first (a compare-and-set).
          dannan::agree::Changed out;
          out.values.assign(current.size(), value);
          return out;
        },
        {}, 10s, io.get_executor(), [&](dannan::agree::Outcome out) { written.set_value(std::move(out)); });
    const auto write = written.get_future().get();
    std::printf("written: %s\n", write.committed ? "yes" : write.why.c_str());

    // Read back on node 2: a strict read through the same replica set.
    std::promise<dannan::agree::Outcome> read;
    nodes[2]->context().agreement().read("example", slot, {name}, genesis, 0, {}, 10s, io.get_executor(),
                                         [&](dannan::agree::Outcome out) { read.set_value(std::move(out)); });
    const auto back = read.get_future().get();
    const bool same = back.committed && !back.values.empty() && back.values.front() == value;
    std::printf("read on another node: %s\n", same ? "hello" : back.why.c_str());
    status = founding.committed && write.committed && same ? 0 : 1;
  }

  for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) (*it)->stop();
  nodes.clear();
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  fs::remove_all(root);
  return status;
}
