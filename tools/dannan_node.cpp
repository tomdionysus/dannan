//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-node: a Dannan node with the transport and nothing above it, for
// the two-process test against a Macha node. It answers ping, members (its
// own record and the peers it has met) and telemetry (an empty set), and
// reports what happens as one line per event on stdout. Given --peer, it
// calls the peer once through: members, ping, a dial_request for the DATA
// lane, and get_object over the lane the peer dials back. It registers
// Macha's message types as Macha will (see routes()), and serves none of them.
//
//   dannan-node --key FILE --port N [--listen ADDR] [--advertise HOST]
//               [--peer HOST:PORT] [--seconds N]

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dannan/net/client.h"
#include "dannan/net/server.h"
#include "tools/macha_types.h"

namespace {

using namespace dannan;
using namespace std::chrono_literals;
namespace asio = boost::asio;

std::mutex output;

void say(const std::string& line) {
  std::lock_guard lock(output);
  std::cout << line << std::endl;
}

std::string short_id(const identity::NodeId& id) { return to_string(id); }

struct Options {
  std::string key;
  std::string listen = "127.0.0.1";
  std::string advertise = "127.0.0.1";
  uint16_t port = 0;
  std::string peer;
  int seconds = 30;
};

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
    else if (option == "--listen")
      out.listen = value();
    else if (option == "--advertise")
      out.advertise = value();
    else if (option == "--port")
      out.port = static_cast<uint16_t>(std::stoi(value()));
    else if (option == "--peer")
      out.peer = value();
    else if (option == "--seconds")
      out.seconds = std::stoi(value());
    else
      throw std::runtime_error("unknown option " + option);
  }
  if (out.key.empty()) throw std::runtime_error("--key is required");
  return out;
}

// Dannan's own types, and Macha's (tools/macha_types.h).
std::shared_ptr<const net::RouteTable> routes() {
  auto table = net::RouteTable::with_dannan_types();
  tools::macha_protocol23_types([&](net::MessageType type, net::Registration registration) { table.add(type, std::move(registration)); });
  return std::make_shared<const net::RouteTable>(std::move(table));
}

net::Bytes empty_telemetry_set() {
  net::Writer writer;
  for (const char c : std::string("MACHTEL3")) writer.u8(static_cast<uint8_t>(c));
  writer.u32(0);
  return writer.take();
}

net::CallOutcome wait_for(const std::function<void(net::Client::Done)>& place) {
  auto done = std::make_shared<std::promise<net::CallOutcome>>();
  auto future = done->get_future();
  place([done](net::CallOutcome outcome) { done->set_value(std::move(outcome)); });
  if (future.wait_for(10s) != std::future_status::ready) return {std::nullopt, "timed out"};
  return future.get();
}

std::string describe(const net::CallOutcome& outcome) {
  if (!outcome.reply) return "failed " + outcome.failure;
  if (const auto refusal = net::refusal_of(*outcome.reply)) return "refused " + std::to_string(refusal->status) + " " + refusal->code;
  return "type " + std::to_string(static_cast<unsigned>(outcome.reply->type));
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
  const auto key = identity::load_cluster_key(options.key);
  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) threads.emplace_back([&] { io.run(); });

  std::mutex roster_mutex;
  membership::NodeRecord self;
  self.id = identity::NodeId::random();
  self.host = options.advertise;
  std::map<identity::NodeId, membership::NodeRecord> met;
  auto meet = [&](const membership::NodeRecord& peer) {
    std::lock_guard lock(roster_mutex);
    met[peer.id] = peer;
  };
  auto local = [&] {
    std::lock_guard lock(roster_mutex);
    return self;
  };

  auto table = routes();
  auto traffic = std::make_shared<net::TransportTraffic>();
  std::unique_ptr<net::Client> client;
  net::ServerOptions server_options;
  server_options.key = key;
  server_options.local = local;
  server_options.routes = table;
  server_options.traffic = traffic;
  net::Server server(
      io, options.listen, options.port, server_options,
      [&](const membership::NodeRecord& peer, sched::FrameType, const net::Message& request) -> net::Handled {
        meet(peer);
        if (request.type == net::message::ping) return net::Handled::processed();
        if (request.type == net::message::members) {
          std::vector<membership::NodeRecord> nodes{local()};
          {
            std::lock_guard lock(roster_mutex);
            for (const auto& [_, node] : met) nodes.push_back(node);
          }
          net::Writer writer;
          writer.u32(static_cast<uint32_t>(nodes.size()));
          for (const auto& node : nodes) membership::encode(writer, node);
          return net::Handled::processed({net::message::members_reply, writer.take(), {}});
        }
        if (request.type == net::message::telemetry) {
          say("telemetry " + short_id(peer.id) + " " + std::to_string(request.payload.size()));
          return net::Handled::processed({net::message::telemetry_reply, empty_telemetry_set(), {}});
        }
        if (request.type == net::message::dial_request) return client->handle_dial_request(peer, request);
        return net::Handled::unrecognised();
      },
      [&](const std::shared_ptr<net::Session>& session) {
        meet(session->peer());
        say("admitted " + short_id(session->peer().id) + " " + net::lane_name(session->lane()));
        client->admit(session);
      });

  net::ClientOptions client_options;
  client_options.key = key;
  client_options.local = local;
  client_options.routes = table;
  client_options.traffic = traffic;
  client_options.heartbeat = 1s;
  client_options.dead_after = 10s;
  client_options.alive = meet;
  client_options.inbound.request = [&](const membership::NodeRecord& peer, net::Frame frame, net::Reply reply) {
    server.scheduler().submit(peer, std::move(frame), std::move(reply));
  };
  client = std::make_unique<net::Client>(io, client_options);
  server.start();
  {
    std::lock_guard lock(roster_mutex);
    self.port = server.bound_port();
  }
  client->start();
  say("listening " + short_id(self.id) + " " + std::to_string(server.bound_port()));

  if (!options.peer.empty()) {
    const auto colon = options.peer.rfind(':');
    const net::Endpoint peer{options.peer.substr(0, colon), static_cast<uint16_t>(std::stoi(options.peer.substr(colon + 1)))};
    const auto executor = io.get_executor();
    auto members = wait_for([&](net::Client::Done done) { client->call(peer, net::message::members, {}, sched::FrameType::control, executor, done); });
    say("members " + describe(members) + " dials " + std::to_string(client->stats().dials));
    std::optional<membership::NodeRecord> them;
    if (members.reply && members.reply->type == net::message::members_reply) {
      net::Reader reader(members.reply->payload);
      const auto count = reader.u32();
      for (uint32_t i = 0; i < count; ++i) {
        const auto node = membership::decode(reader);
        say("member " + short_id(node.id) + (node.id == self.id ? " self" : "") + " " + node.host + ":" + std::to_string(node.port));
        if (node.host == peer.host && node.port == peer.port) them = node;
      }
    }
    say("ping " + describe(wait_for([&](net::Client::Done done) { client->call(peer, net::message::ping, {}, sched::FrameType::control, executor, done); })) +
        " dials " + std::to_string(client->stats().dials) + " retired " + std::to_string(client->stats().retired));
    if (them) {
      net::Writer lane;
      lane.u8(static_cast<uint8_t>(net::Lane::data));
      say("dial_request sent " + std::to_string(client->broadcast({net::message::dial_request, lane.take(), {}})));
      for (int i = 0; i < 100 && !client->has_route(them->id, net::Lane::data); ++i) std::this_thread::sleep_for(50ms);
      say(std::string("data lane ") + (client->has_route(them->id, net::Lane::data) ? "open" : "missing"));
      const auto got =
          wait_for([&](net::Client::Done done) { client->call(*them, net::MessageType{4}, net::Bytes(32, 0), sched::FrameType::foreground, executor, done); });
      say("get_object " + describe(got) + " dials " + std::to_string(client->stats().dials));
    }
    say("script done");
  }

  std::this_thread::sleep_for(std::chrono::seconds(options.seconds));
  client->stop();
  server.stop();
  work.reset();
  io.stop();
  for (auto& thread : threads) thread.join();
  say("stopped");
  return 0;
}
