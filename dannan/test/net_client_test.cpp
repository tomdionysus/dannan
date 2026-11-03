//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The client over loopback TCP, between whole nodes (a server and a client
// sharing a scheduler and traffic): the inventory's transport rows "two
// lanes per peer plus a probe lane; a lane is dialled on demand and reused;
// concurrent cold callers share one dial", "stall notice and no-progress
// deadline per lane", and "executor cancellation on shutdown", with Macha's
// `test_rpc_cluster.cpp` cases as the model.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <fstream>
#include <future>
#include <thread>

#include "dannan/net/client.h"
#include "dannan/net/server.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::net {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using sched::FrameType;

constexpr MessageType put_object{5};
constexpr MessageType slow{200};

struct Io {
  asio::io_context context;
  asio::executor_work_guard<asio::io_context::executor_type> work{context.get_executor()};
  std::vector<std::thread> threads;
  Io() {
    for (int i = 0; i < 4; ++i) threads.emplace_back([this] { context.run(); });
  }
  ~Io() {
    work.reset();
    context.stop();
    for (auto& thread : threads) thread.join();
  }
};

identity::ClusterKey cluster_key() {
  test::TempDir dir;
  std::ofstream(dir.path() / "k") << "client test";
  return identity::load_cluster_key(dir.path() / "k");
}

std::shared_ptr<const RouteTable> table() {
  auto routes = RouteTable::with_dannan_types();
  routes.add(put_object, {"put_object", Lanes::data, Frames::not_control, false});
  routes.add(slow, {"slow", Lanes::control, Frames::control_only, false});
  return std::make_shared<const RouteTable>(std::move(routes));
}

// A whole node on loopback: a server and a client wired as a node wires
// them. `slow` waits on the node's gate, then answers.
struct Node {
  Io& io;
  test::Gate gate;
  membership::NodeRecord self;
  std::unique_ptr<Server> server;
  std::unique_ptr<Client> client;

  explicit Node(Io& io, ClientOptions tuning = {}, std::optional<identity::NodeId> id = std::nullopt) : io(io) {
    self.id = id ? *id : identity::NodeId::random();
    self.host = "127.0.0.1";
    const auto key = cluster_key();
    auto traffic = std::make_shared<TransportTraffic>();
    ServerOptions server_options;
    server_options.key = key;
    server_options.local = [this] { return self; };
    server_options.routes = table();
    server_options.traffic = traffic;
    server = std::make_unique<Server>(
        io.context, "127.0.0.1", 0, server_options,
        [this](const membership::NodeRecord&, FrameType, const Message& request) {
          if (request.type == slow) gate.enter_and_wait();
          return Message{message::ok, request.payload, {}};
        },
        [this](const std::shared_ptr<Session>& session) { client->admit(session); });
    ClientOptions options = std::move(tuning);
    options.key = key;
    options.local = [this] { return self; };
    options.routes = table();
    options.traffic = traffic;
    options.inbound.request = [this](const membership::NodeRecord& peer, Frame frame, Reply reply) {
      server->scheduler().submit(peer, std::move(frame), std::move(reply));
    };
    client = std::make_unique<Client>(io.context, std::move(options));
    server->start();
    self.port = server->bound_port();
  }
  ~Node() {
    gate.open();
    client->stop();
    server->stop();
  }
  Endpoint endpoint() const { return {self.host, self.port}; }
};

CallOutcome call(Node& from, const membership::NodeRecord& to, MessageType type = message::members, FrameType frame = FrameType::control, Bytes payload = {}) {
  auto done = std::make_shared<std::promise<CallOutcome>>();
  auto future = done->get_future();
  from.client->call(to, type, std::move(payload), frame, from.io.context.get_executor(), [done](CallOutcome outcome) { done->set_value(std::move(outcome)); });
  if (future.wait_for(10s) != std::future_status::ready) return {std::nullopt, "test timed out"};
  return future.get();
}

bool eventually(const std::function<bool()>& condition) {
  for (int spin = 0; spin < 1000; ++spin) {
    if (condition()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return condition();
}

TEST(Client, DialsALaneOnDemandAndReusesIt) {
  Io io;
  Node a(io), b(io);
  for (int i = 0; i < 5; ++i) {
    const auto outcome = call(a, b.self);
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->type, message::ok);
  }
  EXPECT_EQ(a.client->stats().dials, 1u);
  EXPECT_EQ(a.client->stats().reused, 4u);
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::control));
  EXPECT_FALSE(a.client->has_route(b.self.id, Lane::data));
  EXPECT_EQ(b.server->sessions(), 1u);
}

// The DATA lane is its own session, opened only when DATA work needs it.
TEST(Client, OpensTheDataLaneOnlyWhenDataWorkNeedsIt) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  EXPECT_FALSE(a.client->has_route(b.self.id, Lane::data));
  const auto outcome = call(a, b.self, put_object, FrameType::foreground, Bytes(100000, 7));
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(outcome.reply->payload, Bytes(100000, 7));
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::data));
  EXPECT_EQ(a.client->stats().dials, 2u);
  EXPECT_TRUE(eventually([&] { return b.server->sessions() == 2; }));
}

// Dialling is expensive: concurrent cold callers for one lane share a dial.
TEST(Client, ConcurrentColdCallersShareOneDial) {
  Io io;
  Node a(io), b(io);
  std::vector<std::shared_ptr<std::promise<CallOutcome>>> done;
  for (int i = 0; i < 24; ++i) {
    done.push_back(std::make_shared<std::promise<CallOutcome>>());
    a.client->call(b.self, put_object, Bytes(1000, 1), FrameType::foreground, io.context.get_executor(),
                   [promise = done.back()](CallOutcome outcome) { promise->set_value(std::move(outcome)); });
  }
  for (auto& promise : done) {
    auto future = promise->get_future();
    ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
    const auto outcome = future.get();
    EXPECT_TRUE(outcome.reply) << outcome.failure;
  }
  EXPECT_EQ(a.client->stats().dials, 1u);
}

// A session works both ways: the node that was dialled answers back over it
// and dials nothing.
TEST(Client, CallsBackOverTheSessionThePeerOpened) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  ASSERT_TRUE(eventually([&] { return b.client->has_route(a.self.id, Lane::control); }));
  const auto outcome = call(b, a.self);
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(b.client->stats().dials, 0u);
  EXPECT_EQ(a.server->sessions(), 0u);
}

// Two nodes dialling each other at once settle on one session per lane, the
// lower id's own; the other is retired, so no call on it fails.
TEST(Client, SimultaneousDialsSettleOnTheLowerIdsSession) {
  Io io;
  for (int round = 0; round < 5; ++round) {
    Node a(io), b(io);
    std::vector<std::future<CallOutcome>> outcomes;
    for (int i = 0; i < 8; ++i) {
      outcomes.push_back(std::async(std::launch::async, [&] { return call(a, b.self); }));
      outcomes.push_back(std::async(std::launch::async, [&] { return call(b, a.self); }));
    }
    for (auto& outcome : outcomes) {
      const auto result = outcome.get();
      EXPECT_TRUE(result.reply) << result.failure;
    }
    const auto& lower = a.self.id < b.self.id ? a : b;
    const auto& higher = a.self.id < b.self.id ? b : a;
    EXPECT_TRUE(eventually([&] { return a.server->sessions() + b.server->sessions() == 1; }));
    if (lower.client->stats().dials && higher.client->stats().dials) {
      // Both dialled: the higher id's dial is the one retired.
      EXPECT_EQ(lower.server->sessions(), 0u);
      EXPECT_EQ(higher.server->sessions(), 1u);
    }
    EXPECT_TRUE(call(a, b.self).reply);
    EXPECT_TRUE(call(b, a.self).reply);
  }
}

// A call that makes no progress for the lane's deadline fails, transiently,
// for the caller to retry; one that is slow but inside it is untouched; zero
// means no deadline.
TEST(Client, ACallWithNoProgressFailsAtTheLanesDeadline) {
  Io io;
  ClientOptions tuning;
  tuning.control_no_progress = 300ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  const auto started = std::chrono::steady_clock::now();
  const auto stalled = call(a, b.self, slow);
  const auto took = std::chrono::steady_clock::now() - started;
  EXPECT_FALSE(stalled.reply);
  EXPECT_NE(stalled.failure.find("cancelled for retry"), std::string::npos) << stalled.failure;
  EXPECT_NE(stalled.failure.find("slow"), std::string::npos) << stalled.failure;
  EXPECT_GE(took, 300ms);
  EXPECT_LT(took, 3s);
  b.gate.open();
  ASSERT_TRUE(call(a, b.self).reply);
}

TEST(Client, NoDeadlineWhenTheLanesIsZero) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  std::thread opener([&] {
    b.gate.wait_for_entries(1);
    std::this_thread::sleep_for(500ms);
    b.gate.open();
  });
  const auto outcome = call(a, b.self, slow);
  opener.join();
  EXPECT_TRUE(outcome.reply) << outcome.failure;
}

// A dial that reaches another node than the one meant is refused.
TEST(Client, ADialAnsweredByAnotherNodeIsRefused) {
  Io io;
  Node a(io), b(io);
  auto impostor = b.self;
  impostor.id = identity::NodeId::random();
  const auto outcome = call(a, impostor);
  EXPECT_FALSE(outcome.reply);
  EXPECT_FALSE(outcome.failure.empty());
  EXPECT_FALSE(a.client->has_route(impostor.id, Lane::control));
}

TEST(Client, ADialToNothingFailsWithTheReason) {
  Io io;
  Node a(io);
  membership::NodeRecord nobody;
  nobody.id = identity::NodeId::random();
  nobody.host = "127.0.0.1";
  {
    Node gone(io);
    nobody.port = gone.self.port;
  }
  const auto outcome = call(a, nobody);
  EXPECT_FALSE(outcome.reply);
  EXPECT_NE(outcome.failure.find("dial"), std::string::npos) << outcome.failure;
}

// A probe proves a node can be dialled and is who it says; it leaves no
// route behind.
TEST(Client, AProbeDialAuthenticatesAndLeavesNoRoute) {
  Io io;
  Node a(io), b(io);
  auto probe = [&](const identity::NodeId& expected) {
    std::promise<std::string> done;
    a.client->probe(b.endpoint(), expected, io.context.get_executor(), [&](std::string error) { done.set_value(std::move(error)); });
    return done.get_future().get();
  };
  EXPECT_EQ(probe(b.self.id), "");
  EXPECT_NE(probe(identity::NodeId::random()), "");
  EXPECT_FALSE(a.client->has_route(b.self.id, Lane::control));
  EXPECT_EQ(b.server->sessions(), 0u);
}

// Stopping fails every pending call at once, so nothing waits on a peer
// through shutdown, and admits no new one.
TEST(Client, StoppingFailsPendingCallsAtOnceAndRefusesNewOnes) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  auto done = std::make_shared<std::promise<CallOutcome>>();
  auto future = done->get_future();
  a.client->call(b.self, slow, {}, FrameType::control, io.context.get_executor(), [done](CallOutcome outcome) { done->set_value(std::move(outcome)); });
  ASSERT_TRUE(b.gate.wait_for_entries(1));
  a.client->stop();
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(future.get().reply);
  const auto refused = call(a, b.self);
  EXPECT_FALSE(refused.reply);
  EXPECT_NE(refused.failure.find("stopped"), std::string::npos) << refused.failure;
}

}  // namespace
}  // namespace dannan::net
