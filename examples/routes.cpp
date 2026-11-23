//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Message types and calls (docs/guide/delivery.md): a type registered on both nodes, before they
// start, with its rules; a handler on one; a call from the other within a deadline, answered; and
// the same type called on a node with no handler bound, answered 501. Exits 0 when all of it
// happened.
//
//   routes [STATE_DIR]

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>

#include "dannan/identity/cluster_key.h"
#include "dannan/node.h"

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// A hosted system's own type: numbered from 1024, a name, its lane, which frame types it may
// travel as, and whether a peer's requests of it run one at a time.
constexpr dannan::net::MessageType greet{1500};
const dannan::net::Registration greet_rules{"greet", dannan::net::Lanes::control, dannan::net::Frames::any, false};

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
  config.version = "routes-example";
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
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dannan-example-routes";
  fs::remove_all(root);
  fs::create_directories(root);
  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&io] { io.run(); });
  std::ofstream(root / "cluster.key") << "an example cluster's key, made for this run";
  const auto key = dannan::identity::load_cluster_key(root / "cluster.key");

  // The answering node: the type and its handler, registered before it starts.
  dannan::Node answering(io, settings(root / "answering", key, {}));
  answering.context().routes().add(greet, greet_rules,
                                   [](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
                                     const std::string name(request.payload.begin(), request.payload.end());
                                     const std::string reply = "hello, " + name;
                                     return dannan::net::Handled::processed({dannan::net::message::ok, dannan::net::Bytes(reply.begin(), reply.end()), {}});
                                   });
  answering.start();

  // The calling node: the type's rules too, with no handler, so it knows the type's lane.
  dannan::Node calling(io, settings(root / "calling", key, {{"127.0.0.1", answering.port()}}));
  calling.context().routes().add(greet, greet_rules);
  calling.start();

  int status = 1;
  const auto answering_id = answering.context().identity().id();
  std::optional<dannan::membership::NodeRecord> peer;
  wait_until([&] {
    for (const auto& record : calling.context().nodes().active())
      if (record.id == answering_id) peer = record;
    return peer.has_value();
  });
  if (!peer) {
    std::fprintf(stderr, "the nodes did not find each other\n");
  } else {
    // A call: the reply, or why none came, posted to the executor, within the deadline.
    std::promise<dannan::net::CallOutcome> answered;
    const std::string name = "example";
    calling.context().routes().call(*peer, greet, dannan::net::Bytes(name.begin(), name.end()), dannan::sched::FrameType::control, 5s, io.get_executor(),
                                    [&](dannan::net::CallOutcome outcome) { answered.set_value(std::move(outcome)); });
    const auto outcome = answered.get_future().get();
    const bool replied = outcome.reply && !dannan::net::refusal_of(*outcome.reply);
    const auto text = replied ? std::string(outcome.reply->payload.begin(), outcome.reply->payload.end()) : outcome.failure;
    std::printf("call: %s\n", text.c_str());

    // The same type called the other way: the calling node registered it with no handler, so it
    // is answered 501 "not handled here", a refusal, not a failure to reach it.
    std::optional<dannan::membership::NodeRecord> back;
    for (const auto& record : answering.context().nodes().active())
      if (record.id == calling.context().identity().id()) back = record;
    bool refused = false;
    if (back) {
      const auto unhandled = answering.context().routes().call_and_wait(*back, greet, {}, dannan::sched::FrameType::control, 5s);
      const auto refusal = unhandled.reply ? dannan::net::refusal_of(*unhandled.reply) : std::nullopt;
      refused = refusal && refusal->status == 501;
      std::printf("unhandled: %s\n", refused ? "refused 501" : "not refused");
    }
    status = replied && text == "hello, example" && refused ? 0 : 1;
  }

  calling.stop();
  answering.stop();
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  fs::remove_all(root);
  return status;
}
