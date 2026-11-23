//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Each delivery kind (docs/guide/delivery.md): a channel of each kind opened on two nodes, a
// subscriber on one by topic prefix, publishes from the other. notify, ordered and stream are
// published and return where they went; bulk is acknowledged by each destination. Exits 0 when
// every kind delivered.
//
//   delivery [STATE_DIR]

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "dannan/identity/cluster_key.h"
#include "dannan/node.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

dannan::NodeConfig settings(const fs::path& state, const dannan::identity::ClusterKey& key, std::vector<dannan::net::Endpoint> seeds) {
  dannan::NodeConfig config;
  config.state_path = state;
  config.key = key;
  config.listen_host = "127.0.0.1";
  config.advertise_host = "127.0.0.1";
  config.port = 0;
  config.inbound = dannan::reach::Inbound::yes;
  config.bootstrap = std::move(seeds);
  config.heartbeat = 200ms;
  config.version = "delivery-example";
  return config;
}

bool wait_until(const std::function<bool()>& done, std::chrono::seconds bound = 30s) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= until) return false;
    std::this_thread::sleep_for(20ms);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dannan-example-delivery";
  fs::remove_all(root);
  fs::create_directories(root);
  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&io] { io.run(); });
  std::ofstream(root / "cluster.key") << "an example cluster's key, made for this run";
  const auto key = dannan::identity::load_cluster_key(root / "cluster.key");

  dannan::Node publisher(io, settings(root / "publisher", key, {}));
  publisher.start();
  dannan::Node subscriber(io, settings(root / "subscriber", key, {{"127.0.0.1", publisher.port()}}));
  subscriber.start();

  // Every node that publishes or subscribes opens the channel alike, with its delivery kind.
  const std::map<std::string, dannan::net::Delivery> kinds{{"gauges", dannan::net::Delivery::notify},
                                                           {"telemetry", dannan::net::Delivery::ordered},
                                                           {"orders", dannan::net::Delivery::stream},
                                                           {"blobs", dannan::net::Delivery::bulk}};
  for (auto* node : {&publisher, &subscriber})
    for (const auto& [channel, kind] : kinds) node->context().channels().open(channel, kind, kind == dannan::net::Delivery::stream ? 1 << 20 : 0);

  int status = 1;
  const auto subscriber_id = subscriber.context().identity().id();
  if (!wait_until([&] {
        for (const auto& record : publisher.context().nodes().active())
          if (record.id == subscriber_id) return true;
        return false;
      })) {
    std::fprintf(stderr, "the nodes did not find each other\n");
  } else {
    // Subscribe by topic prefix, on a strand so each channel's deliveries arrive in order. Subscribing
    // with a deadline pushes the interest to every peer at once and says who holds it, so a publish
    // made after it reaches this subscriber.
    std::mutex mutex;
    std::map<std::string, std::vector<std::string>> got;
    auto strand = boost::asio::make_strand(io);
    for (const auto& [channel, kind] : kinds) {
      std::promise<dannan::membership::Pushed> pushed;
      subscriber.context().channels().subscribe(
          channel, "example/", strand,
          [&, name = channel](dannan::net::ChannelMessage message) {
            std::lock_guard lock(mutex);
            got[name].push_back(message.topic + "=" + std::string(message.payload.begin(), message.payload.end()));
          },
          5s, io.get_executor(), [&](dannan::membership::Pushed out) { pushed.set_value(std::move(out)); });
      const auto holds = pushed.get_future().get();
      std::printf("%s: interest held by %zu peer(s)\n", channel.c_str(), holds.held.size());
    }

    // notify, ordered and stream: sent now; where each went is the evidence.
    for (const auto* channel : {"gauges", "telemetry", "orders"}) {
      for (int i = 1; i <= 3; ++i) {
        const auto payload = std::to_string(i);
        const auto went =
            publisher.context().channels().publish(channel, "example/" + std::string(channel), dannan::net::Bytes(payload.begin(), payload.end()));
        if (!went.missed.empty()) std::printf("%s: missed %zu\n", channel, went.missed.size());
      }
    }
    // bulk: each destination acknowledges, within the deadline.
    std::promise<dannan::net::Published> acknowledged;
    publisher.context().channels().publish_bulk("blobs", "example/blob", dannan::net::Bytes(64 * 1024, 7), 5s, io.get_executor(),
                                                [&](dannan::net::Published out) { acknowledged.set_value(std::move(out)); });
    const auto bulk = acknowledged.get_future().get();
    std::printf("blobs: acknowledged by %zu destination(s)\n", bulk.to.size());

    const bool all = wait_until([&] {
      std::lock_guard lock(mutex);
      return got["gauges"].size() == 3 && got["telemetry"].size() == 3 && got["orders"].size() == 3 && got["blobs"].size() == 1;
    });
    {
      std::lock_guard lock(mutex);
      for (const auto& [channel, messages] : got) {
        std::printf("%s received:", channel.c_str());
        for (const auto& message : messages) std::printf(" %s", message.size() > 40 ? "(64 KiB)" : message.c_str());
        std::printf("\n");
      }
    }
    status = all && !bulk.to.empty() ? 0 : 1;
  }

  subscriber.stop();
  publisher.stop();
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  fs::remove_all(root);
  return status;
}
