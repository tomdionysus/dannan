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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <boost/asio/io_context.hpp>
#include <cstdio>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

#include "dannan/net/client.h"
#include "dannan/net/server.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/recording_sink.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::net {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using sched::FrameType;

constexpr MessageType put_object{2005};
constexpr MessageType slow{1200};
constexpr MessageType sluggish{1201};
constexpr MessageType news{1202};
constexpr MessageType sluggish_throw{1203};
constexpr MessageType hurried{1204};

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
  routes.add(sluggish, {"sluggish", Lanes::control, Frames::control_only, false});
  routes.add(news, {"news", Lanes::control, Frames::any, true});
  routes.add(sluggish_throw, {"sluggish_throw", Lanes::control, Frames::control_only, false});
  Registration quick{"hurried", Lanes::control, Frames::control_only, false};
  quick.no_progress = 100ms;
  routes.add(hurried, quick);
  return std::make_shared<const RouteTable>(std::move(routes));
}

// A whole node on loopback: a server and a client wired as a node wires
// them. `slow` waits on the node's gate, then answers; `sluggish` takes
// 300 ms; the first `hang_pings` pings wait on the gate; `news` is counted.
struct Node {
  Io& io;
  test::Gate gate;
  std::atomic<int> hang_pings{0};
  std::atomic<int> heard{0};
  std::atomic<bool> ignore_dial_requests{false};
  std::atomic<bool> throw_on_news{false};
  std::atomic<bool> hold_data{false};
  std::function<void(size_t)> on_put;
  std::mutex held_mutex;
  std::vector<std::shared_ptr<Session>> held_inbound;
  // Sessions that arrived while adopt_inbound was off become routes now, as they
  // would have a moment later.
  void release_inbound() {
    adopt_inbound = true;
    std::vector<std::shared_ptr<Session>> held;
    {
      std::lock_guard lock(held_mutex);
      held.swap(held_inbound);
    }
    for (const auto& session : held) client->admit(session);
  }
  std::atomic<int> slow_started{0};
  std::atomic<int> slow_finished{0};
  std::atomic<bool> slow_data{false};
  // Whether the sessions peers open become this client's routes too.
  std::atomic<bool> adopt_inbound{true};
  membership::NodeRecord self;
  std::unique_ptr<Server> server;
  std::unique_ptr<Client> client;

  explicit Node(Io& io, ClientOptions tuning = {}, uint32_t capabilities = membership::capabilities_default) : io(io) {
    self.id = identity::NodeId::random();
    self.host = "127.0.0.1";
    self.capabilities = capabilities;
    const auto key = cluster_key();
    auto traffic = std::make_shared<TransportTraffic>();
    ServerOptions server_options;
    server_options.credentials = key;
    server_options.local = [this] { return self; };
    server_options.routes = table();
    server_options.traffic = traffic;
    server_options.log = tuning.log;
    server = std::make_unique<Server>(
        io.context, "127.0.0.1", 0, server_options,
        [this](const membership::NodeRecord& peer, FrameType, const Message& request) {
          if (request.type == message::dial_request) return ignore_dial_requests ? Handled::processed() : client->handle_dial_request(peer, request);
          if (request.type == slow) ++slow_started;
          if (request.type == slow || request.type == hurried || (request.type == put_object && hold_data)) gate.enter_and_wait();
          if (request.type == slow) ++slow_finished;
          if (request.type == put_object && slow_data) std::this_thread::sleep_for(300ms);
          if (request.type == put_object && on_put) on_put(request.payload.size());
          if (request.type == sluggish_throw && throw_on_news) throw std::runtime_error("the handler broke");
          if (request.type == sluggish) std::this_thread::sleep_for(300ms);
          if (request.type == news) ++heard;
          if (request.type == message::ping && hang_pings.fetch_sub(1) > 0) gate.enter_and_wait();
          return Handled::processed({message::ok, request.payload, {}});
        },
        [this](const std::shared_ptr<Session>& session) {
          if (adopt_inbound) {
            client->admit(session);
          } else {
            std::lock_guard lock(held_mutex);
            held_inbound.push_back(session);
          }
        });
    ClientOptions options = std::move(tuning);
    options.credentials = key;
    options.local = [this] { return self; };
    options.routes = table();
    options.traffic = traffic;
    options.inbound.request = [this](const membership::NodeRecord& peer, Frame frame, Reply reply) {
      server->scheduler().submit(peer, std::move(frame), std::move(reply));
    };
    client = std::make_unique<Client>(io.context, std::move(options));
    server->start();
    self.port = server->bound_port();
    client->start();
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
  if (future.wait_for(10s) != std::future_status::ready) return {std::nullopt, "test timed out", {}};
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

// A session retired off the route table may still hold a call waiting for
// its answer; stopping the client fails that call at once too (found by
// the 0.2.0 sanitizer run: it waited out its deadline).
TEST(Client, StoppingFailsACallOnARetiredSession) {
  Io io;
  Node a(io), b(io);
  auto& higher = a.self.id < b.self.id ? b : a;
  auto& lower = a.self.id < b.self.id ? a : b;
  // The higher id dials and leaves a call blocked at the lower id, which
  // does not take that session as a route of its own.
  lower.adopt_inbound = false;
  auto done = std::make_shared<std::promise<CallOutcome>>();
  auto pending = done->get_future();
  higher.client->call(lower.self, slow, {}, FrameType::control, io.context.get_executor(),
                      [done](CallOutcome outcome) { done->set_value(std::move(outcome)); });
  ASSERT_TRUE(lower.gate.wait_for_entries(1));
  // So the lower id dials back, and both ends keep its session: the
  // higher's, holding the blocked call, is retired off the route table.
  ASSERT_TRUE(call(lower, higher.self).reply);
  ASSERT_TRUE(eventually([&] { return higher.client->stats().retired > 0; }));
  const auto started = std::chrono::steady_clock::now();
  higher.client->stop();
  ASSERT_EQ(pending.wait_for(3s), std::future_status::ready) << "the call on the retired session outlived stop()";
  EXPECT_FALSE(pending.get().reply);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
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

// The round trip is smoothed from pings alone, so slow handlers and large
// payloads never distort it.
TEST(Health, TheRoundTripComesFromPingsNotFromSlowReplies) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 50ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(call(a, b.self, sluggish).reply);
  ASSERT_TRUE(eventually([&] { return a.client->peer_latency(b.self.id).has_value(); }));
  EXPECT_LT(*a.client->peer_latency(b.self.id), 150ms);
}

// A ping that hangs is abandoned after a third of the liveness budget and
// retried; the peer keeps its route.
TEST(Health, AHungPingIsRetriedInsideTheLivenessBudget) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 100ms;
  tuning.dead_after = 900ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.hang_pings = 1;
  ASSERT_TRUE(b.gate.wait_for_entries(1));
  ASSERT_TRUE(eventually([&] { return a.client->peer_latency(b.self.id).has_value(); }));
  std::this_thread::sleep_for(1200ms);
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::control));
  EXPECT_EQ(b.server->sessions(), 1u);
}

// A peer that answers no ping for the liveness budget loses its route.
TEST(Health, APeerThatStopsAnsweringLosesItsRoute) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 100ms;
  tuning.dead_after = 600ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.hang_pings = 1000;
  EXPECT_TRUE(eventually([&] { return !a.client->has_route(b.self.id, Lane::control); }));
  EXPECT_TRUE(eventually([&] { return b.server->sessions() == 0; }));
}

// The health loop's decisions read the injected time: with a real clock these
// budgets would take an hour.
TEST(Health, TheHeartbeatFollowsTheInjectedTime) {
  Io io;
  test::SteppedTime time;
  ClientOptions tuning;
  tuning.time = &time;
  tuning.heartbeat = 1h;
  tuning.dead_after = 3h;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  EXPECT_FALSE(a.client->peer_latency(b.self.id).has_value());
  time.advance(1h);
  EXPECT_TRUE(eventually([&] { return a.client->peer_latency(b.self.id).has_value(); }));
}

TEST(Health, ALivenessBudgetIsSpentInTheInjectedTime) {
  Io io;
  test::SteppedTime time;
  ClientOptions tuning;
  tuning.time = &time;
  tuning.heartbeat = 1h;
  tuning.dead_after = 3h;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.hang_pings = 1000;
  time.advance(1h);
  ASSERT_TRUE(b.gate.wait_for_entries(1));
  // Time standing still spends none of the budget, however often it is read.
  const auto reads = time.reads();
  ASSERT_TRUE(eventually([&] { return time.reads() > reads + 20; }));
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::control));
  EXPECT_TRUE(eventually([&] {
    time.advance(5min);
    return !a.client->has_route(b.self.id, Lane::control);
  }));
}

TEST(Health, StoppingEndsAProbeWaitingOnStoppedTime) {
  Io io;
  test::SteppedTime time;
  ClientOptions tuning;
  tuning.time = &time;
  tuning.heartbeat = 1h;
  tuning.dead_after = 3h;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.hang_pings = 1000;
  time.advance(1h);
  ASSERT_TRUE(b.gate.wait_for_entries(1));
  a.client->stop();
  EXPECT_FALSE(a.client->has_route(b.self.id, Lane::control));
}

// The watchdog decides by the injected time too, though a timer wakes it.
TEST(Client, TheNoProgressDeadlineFollowsTheInjectedTime) {
  Io io;
  test::SteppedTime time;
  ClientOptions tuning;
  tuning.time = &time;
  tuning.heartbeat = 2h;
  tuning.dead_after = 6h;
  tuning.control_no_progress = 1h;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  std::thread stepper([&] {
    b.gate.wait_for_entries(1);
    time.advance(1h);
  });
  const auto stalled = call(a, b.self, slow);
  stepper.join();
  EXPECT_FALSE(stalled.reply);
  EXPECT_NE(stalled.failure.find("cancelled for retry"), std::string::npos) << stalled.failure;
}

// A slow call outlives the liveness budget while pings answer: health asks of
// the peer, not of the call, on either lane.
TEST(Health, ASlowCallOutlivesTheLivenessBudgetWhilePingsAnswer) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 50ms;
  tuning.dead_after = 300ms;
  Node a(io, tuning), b(io);
  b.hold_data = true;
  ASSERT_TRUE(call(a, b.self).reply);
  std::thread opener([&] {
    b.gate.wait_for_entries(2);
    std::this_thread::sleep_for(1s);
    b.gate.open();
  });
  auto data = std::async(std::launch::async, [&] { return call(a, b.self, put_object, FrameType::foreground, Bytes(100, 1)); });
  const auto control = call(a, b.self, slow);
  EXPECT_TRUE(control.reply) << control.failure;
  const auto bulk = data.get();
  EXPECT_TRUE(bulk.reply) << bulk.failure;
  opener.join();
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::control));
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::data));
}

// Every hung ping is given up on at a third of the budget and sent again, so a
// peer gets three chances before its route goes.
TEST(Health, EveryHungPingIsRetriedAtAThirdOfTheBudget) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 200ms;
  tuning.dead_after = 900ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.hang_pings = 100000;
  const auto before = a.client->stats().pings;
  ASSERT_TRUE(eventually([&] { return !a.client->has_route(b.self.id, Lane::control); }));
  EXPECT_GE(a.client->stats().pings, before + 3);
}

// However late, an answer is proof of life. The first ping is held past its
// give-up and released: the route stays and the peer goes on being measured.
TEST(Health, ALateAnswerKeepsTheRoute) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 100ms;
  tuning.dead_after = 900ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.hang_pings = 1;
  ASSERT_TRUE(b.gate.wait_for_entries(1));
  std::this_thread::sleep_for(400ms);
  b.gate.open();
  ASSERT_TRUE(eventually([&] { return a.client->peer_latency(b.self.id).has_value(); }));
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::control));
}

TEST(Client, ASlowCallInsideItsDeadlineSucceeds) {
  Io io;
  ClientOptions tuning;
  tuning.control_no_progress = 3s;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  const auto outcome = call(a, b.self, sluggish);
  EXPECT_TRUE(outcome.reply) << outcome.failure;
}

TEST(Client, TheDataLanesDeadlineBindsOnlyDataCalls) {
  Io io;
  ClientOptions tuning;
  tuning.data_no_progress = 100ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  EXPECT_TRUE(call(a, b.self, sluggish).reply);
  b.hold_data = true;
  const auto held = call(a, b.self, put_object, FrameType::foreground, Bytes(100, 1));
  EXPECT_FALSE(held.reply);
  EXPECT_NE(held.failure.find("cancelled for retry"), std::string::npos) << held.failure;
}

TEST(Client, TheControlLanesDeadlineBindsOnlyControlCalls) {
  Io io;
  ClientOptions tuning;
  tuning.control_no_progress = 100ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.slow_data = true;
  EXPECT_TRUE(call(a, b.self, put_object, FrameType::foreground, Bytes(100, 1)).reply);
  EXPECT_FALSE(call(a, b.self, sluggish).reply);
}

// One call's watch beats its type's and its lane's, both ways.
TEST(Client, ACallsOwnWatchReplacesItsTypesAndItsLanes) {
  Io io;
  ClientOptions tuning;
  tuning.control_no_progress = 100ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  const auto watched = [&](MessageType type, CallWatch watch) {
    std::promise<CallOutcome> done;
    auto future = done.get_future();
    a.client->call(b.self, type, {}, FrameType::control, io.context.get_executor(), [&](CallOutcome outcome) { done.set_value(std::move(outcome)); }, watch);
    return future.get();
  };
  // Off: the lane's 100 ms does not apply to a 300 ms call.
  EXPECT_TRUE(watched(sluggish, {0ms, std::nullopt}).reply);
  // Longer: a type with its own 100 ms, held 500 ms, given 5 s for this call.
  std::thread opener([&] {
    b.gate.wait_for_entries(1);
    std::this_thread::sleep_for(500ms);
    b.gate.open();
  });
  EXPECT_TRUE(watched(hurried, {5s, std::nullopt}).reply);
  opener.join();
}

// Work a closed session left in the queue is not started, as its asker has gone;
// work already running finishes, and its reply goes nowhere.
TEST(Server, AClosedSessionsQueuedWorkIsNotStartedAndItsRunningWorkFinishes) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  const auto ask = [&] { a.client->call(b.self, slow, Bytes(10, 1), FrameType::control, io.context.get_executor(), [](CallOutcome) {}); };
  ask();
  ask();
  ASSERT_TRUE(b.gate.wait_for_entries(2));  // both control workers
  ask();
  ASSERT_TRUE(eventually([&] { return b.server->work_stats().control_pending_bytes >= 10; }));
  a.client->stop();
  ASSERT_TRUE(eventually([&] { return b.server->sessions() == 0; }));
  b.gate.open();
  ASSERT_TRUE(eventually([&] { return b.server->work_stats().control_pending_bytes == 0 && b.slow_finished == 2; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(b.slow_started.load(), 2);
  EXPECT_EQ(b.slow_finished.load(), 2);
  // The server is none the worse.
  Node c(io);
  EXPECT_TRUE(call(c, b.self).reply);
}

// The transport under Macha's load: 32 MiB in 4 KiB frames, nothing else waiting
// for it. A later foreground call reaches the handler first, a promoted small call
// overtakes on both sides, and a cancel leaves the session up with no new dial.
TEST(Client, ALargeTransferIsOvertakenCancelledAndPromotedEndToEnd) {
  constexpr size_t big = 32 * 1024 * 1024;
  Io io;
  ClientOptions tuning;
  tuning.max_frame = 4096;
  Node a(io, tuning), b(io);
  std::mutex mutex;
  std::vector<size_t> arrivals;
  b.on_put = [&](size_t bytes) {
    std::lock_guard lock(mutex);
    arrivals.push_back(bytes);
  };
  const auto put = [&](size_t bytes, FrameType frame, std::promise<CallOutcome>& done) {
    return a.client->call(b.self, put_object, Bytes(bytes, 7), frame, io.context.get_executor(),
                          [&done](CallOutcome outcome) { done.set_value(std::move(outcome)); });
  };
  ASSERT_TRUE(call(a, b.self, put_object, FrameType::foreground, Bytes(10, 1)).reply);
  arrivals.clear();
  const auto dials = a.client->stats().dials;
  {
    std::promise<CallOutcome> large, small;
    (void)put(big, FrameType::speculative, large);
    (void)put(100, FrameType::foreground, small);
    auto small_done = small.get_future();
    auto large_done = large.get_future();
    ASSERT_EQ(small_done.wait_for(60s), std::future_status::ready);
    EXPECT_TRUE(small_done.get().reply);
    ASSERT_EQ(large_done.wait_for(120s), std::future_status::ready);
    const auto outcome = large_done.get();
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->payload.size(), big);
    std::lock_guard lock(mutex);
    ASSERT_EQ(arrivals.size(), 2u);
    EXPECT_EQ(arrivals[0], 100u);
    EXPECT_EQ(arrivals[1], big);
    arrivals.clear();
  }
  {
    std::promise<CallOutcome> large, small;
    (void)put(big, FrameType::speculative, large);
    auto promoted = put(100, FrameType::speculative, small);
    promoted->promote(FrameType::foreground);
    auto small_done = small.get_future();
    auto large_done = large.get_future();
    ASSERT_EQ(small_done.wait_for(60s), std::future_status::ready);
    EXPECT_TRUE(small_done.get().reply);
    ASSERT_EQ(large_done.wait_for(120s), std::future_status::ready);
    EXPECT_TRUE(large_done.get().reply);
    std::lock_guard lock(mutex);
    ASSERT_EQ(arrivals.size(), 2u);
    EXPECT_EQ(arrivals[0], 100u);
  }
  {
    std::promise<CallOutcome> large;
    auto handle = put(big, FrameType::speculative, large);
    handle->cancel();
    auto large_done = large.get_future();
    ASSERT_EQ(large_done.wait_for(60s), std::future_status::ready);
    EXPECT_FALSE(large_done.get().reply);
  }
  EXPECT_TRUE(call(a, b.self, put_object, FrameType::foreground, Bytes(10, 1)).reply);
  EXPECT_EQ(a.client->stats().dials, dials);
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::data));
}

TEST(Client, AFastCallPassesAHeldOneOnASession) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  auto held = std::async(std::launch::async, [&] { return call(a, b.self, slow); });
  ASSERT_TRUE(b.gate.wait_for_entries(1));
  EXPECT_TRUE(call(a, b.self).reply);
  EXPECT_TRUE(call(a, b.self, message::ping).reply);
  b.gate.open();
  EXPECT_TRUE(held.get().reply);
}

TEST(Client, ALargeReplyLeavesTheSessionReusable) {
  constexpr size_t large = 4 * 1024 * 1024 + 36;
  Io io;
  Node a(io), b(io);
  for (int i = 0; i < 2; ++i) {
    const auto outcome = call(a, b.self, put_object, FrameType::foreground, Bytes(large, static_cast<uint8_t>(i + 1)));
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->payload, Bytes(large, static_cast<uint8_t>(i + 1)));
  }
  EXPECT_EQ(a.client->stats().dials, 1u);
}

TEST(Client, TheDialledNodeReusesTheDataLane) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self, put_object, FrameType::foreground, Bytes(10, 1)).reply);
  ASSERT_TRUE(eventually([&] { return b.client->has_route(a.self.id, Lane::data); }));
  const auto outcome = call(b, a.self, put_object, FrameType::foreground, Bytes(10, 2));
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(b.client->stats().dials, 0u);
}

TEST(Client, ASecondNameForOnePeerEndsInOneRoute) {
  Io io;
  Node a(io), b(io);
  const auto by = [&](const std::string& host) {
    std::promise<CallOutcome> done;
    auto future = done.get_future();
    (void)a.client->call(Endpoint{host, b.self.port}, message::members, {}, FrameType::control, io.context.get_executor(),
                         [&](CallOutcome outcome) { done.set_value(std::move(outcome)); });
    return future.get();
  };
  const auto numeric = by("127.0.0.1");
  ASSERT_TRUE(numeric.reply) << numeric.failure;
  const auto alias = by("localhost");
  ASSERT_TRUE(alias.reply) << alias.failure;
  EXPECT_TRUE(eventually([&] { return b.server->sessions() == 1; }));
  EXPECT_TRUE(a.client->has_route(b.self.id, Lane::control));
}

// Accepted, then silence: a shared dial ends at the connect timeout, for every
// caller sharing it.
TEST(Client, ASharedDialToASilentListenerEndsWithinTheConnectTimeout) {
  Io io;
  ClientOptions tuning;
  tuning.connect_timeout = 200ms;
  Node a(io, tuning);
  asio::ip::tcp::acceptor silent(io.context, {asio::ip::address_v4::loopback(), 0});
  asio::ip::tcp::socket held(io.context);
  silent.async_accept(held, [](const boost::system::error_code&) {});
  membership::NodeRecord quiet;
  quiet.id = identity::NodeId::random();
  quiet.host = "127.0.0.1";
  quiet.port = silent.local_endpoint().port();
  const auto started = std::chrono::steady_clock::now();
  auto first = std::async(std::launch::async, [&] { return call(a, quiet); });
  auto second = std::async(std::launch::async, [&] { return call(a, quiet); });
  EXPECT_FALSE(first.get().reply);
  EXPECT_FALSE(second.get().reply);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 8s);
  EXPECT_EQ(a.client->stats().dials, 1u);
}

TEST(Broadcast, BestEffortAndNotifyTakeBothRouteDirections) {
  Io io;
  Node a(io), b(io), c(io);
  ASSERT_TRUE(call(b, a.self).reply);
  ASSERT_TRUE(call(a, c.self).reply);
  ASSERT_TRUE(eventually([&] { return a.client->has_route(b.self.id, Lane::control); }));
  for (const auto frame : {FrameType::speculative, FrameType::control}) {
    b.heard = 0;
    c.heard = 0;
    EXPECT_EQ(a.client->broadcast_best_effort({news, {}, {}}, frame), 2u);
    EXPECT_TRUE(eventually([&] { return b.heard == 1 && c.heard == 1; }));
    EXPECT_EQ(a.client->broadcast_notify({news, {}, {}}, frame), 2u);
    EXPECT_TRUE(eventually([&] { return b.heard == 2 && c.heard == 2; }));
  }
}

TEST(Broadcast, AKnownPeerWithNoRouteIsNotDialled) {
  Io io;
  Node a(io), b(io), c(io);
  ASSERT_TRUE(call(a, b.self).reply);
  a.client->note_peer(c.self);
  const auto dials = a.client->stats().dials;
  EXPECT_EQ(a.client->broadcast_best_effort({news, {}, {}}, FrameType::speculative), 1u);
  EXPECT_EQ(a.client->stats().dials, dials);
  EXPECT_FALSE(a.client->has_route(c.self.id, Lane::control));
}

TEST(ReverseDial, TheHubUsesARedialledLaneWithoutAsking) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 50ms;
  Node site(io, tuning, 0), hub(io);
  site.client->set_maintained_peers([&] { return std::vector<membership::NodeRecord>{hub.self}; });
  ASSERT_TRUE(eventually([&] { return hub.client->has_route(site.self.id, Lane::data); }));
  const auto outcome = call(hub, site.self, put_object, FrameType::foreground, Bytes(10, 3));
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(hub.client->stats().dial_requests_sent, 0u);
}

// Finds this process's sockets connected to `port`, without a hook into the session.
std::vector<int> sockets_connected_to(uint16_t port) {
  std::vector<int> out;
  for (int fd = 3; fd < 4096; ++fd) {
    sockaddr_in peer{};
    socklen_t length = sizeof(peer);
    int type = 0;
    socklen_t type_length = sizeof(type);
    if (::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_length) != 0 || type != SOCK_STREAM) continue;
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &length) == 0 && peer.sin_family == AF_INET && ntohs(peer.sin_port) == port) out.push_back(fd);
  }
  return out;
}

TEST(Client, ItsOwnSocketsKeepAliveWithoutNagle) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  ASSERT_TRUE(call(a, b.self, put_object, FrameType::foreground, Bytes(10, 1)).reply);
  const auto sockets = sockets_connected_to(b.self.port);
  // Both lanes: a dial's own ends are the only sockets whose peer is that port.
  ASSERT_GE(sockets.size(), 2u);
  for (const int fd : sockets) {
    int value = 0;
    socklen_t length = sizeof(value);
    ASSERT_EQ(::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &length), 0);
    EXPECT_NE(value, 0);
    ASSERT_EQ(::getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, &length), 0);
    EXPECT_NE(value, 0);
  }
}

// A refusal is an answer: the session it came on carries on.
TEST(Server, ASessionOutlivesARefusal) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  const auto dials = a.client->stats().dials;
  b.throw_on_news = true;
  const auto refused = call(a, b.self, sluggish_throw);
  ASSERT_TRUE(refused.reply);
  EXPECT_EQ(refused.reply->type, message::error);
  EXPECT_TRUE(call(a, b.self).reply);
  EXPECT_EQ(a.client->stats().dials, dials);
  EXPECT_EQ(b.server->sessions(), 1u);
}

// A peer that authenticated and stopped reading backs up: broadcasts skip it and
// reach the others, without waiting.
TEST(Broadcast, ABackedUpPeerIsSkippedWithoutWaiting) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  asio::ip::tcp::acceptor listener(io.context, {asio::ip::address_v4::loopback(), 0});
  membership::NodeRecord silent;
  silent.id = identity::NodeId::random();
  silent.host = "127.0.0.1";
  silent.port = listener.local_endpoint().port();
  auto held = std::make_shared<Session::Socket>(io.context);
  std::promise<void> shaken;
  listener.async_accept([&, held](const boost::system::error_code&, asio::ip::tcp::socket tcp) {
    *held = Session::Socket(io.context, Session::Socket::protocol_type(AF_INET, SOCK_STREAM), tcp.release());
    async_accept_handshake(*held, cluster_key(), silent, 256 * 1024, {"127.0.0.1", 1}, [&](std::exception_ptr, Established) { shaken.set_value(); });
  });
  // 32 MiB to a peer that reads nothing: more than the socket's buffers hold.
  (void)a.client->call(silent, slow, Bytes(32 * 1024 * 1024, 1), FrameType::control, io.context.get_executor(), [](CallOutcome) {});
  ASSERT_EQ(shaken.get_future().wait_for(10s), std::future_status::ready);
  ASSERT_TRUE(eventually([&] { return a.client->has_route(silent.id, Lane::control); }));
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(a.client->broadcast_best_effort({news, {}, {}}, FrameType::speculative), 1u);
  EXPECT_EQ(a.client->broadcast_notify({news, {}, {}}, FrameType::speculative), 1u);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  EXPECT_TRUE(eventually([&] { return b.heard == 2; }));
}

// A simultaneous connect: the lower id keeps its own dial and retires the other, so
// the higher id's call can find its dial retired before the peer's session is its
// route. Forced here, as Macha's case forces it: the call waits for that session,
// and does not dial again.
TEST(Client, ACallOnADialRetiredBeforePlacementWaitsForThePeer) {
  Io io;
  std::atomic<bool> retire{false};
  ClientOptions tuning;
  tuning.place_hook = [&](Session& session) {
    if (retire.exchange(false)) session.retire();
  };
  // Decide which of two is the higher id before tuning goes to it.
  Node a(io, tuning), b(io, tuning);
  auto& low = a.self.id < b.self.id ? a : b;
  auto& high = a.self.id < b.self.id ? b : a;
  high.adopt_inbound = false;
  ASSERT_TRUE(call(low, high.self).reply);
  ASSERT_FALSE(high.client->has_route(low.self.id, Lane::control));
  retire = true;
  auto outcome = std::async(std::launch::async, [&] { return call(high, low.self); });
  ASSERT_TRUE(eventually([&] { return !retire.load(); }));
  // It waits: nothing answers while the peer's session is held.
  EXPECT_EQ(outcome.wait_for(200ms), std::future_status::timeout);
  EXPECT_EQ(high.client->stats().dials, 1u);
  high.release_inbound();
  const auto answered = outcome.get();
  EXPECT_TRUE(answered.reply) << answered.failure;
  EXPECT_EQ(high.client->stats().dials, 1u);
}

// With no session ever coming, the wait ends at the connect timeout and says why.
TEST(Client, AWaitForThePeersSessionIsBounded) {
  Io io;
  std::atomic<bool> retire{false};
  ClientOptions tuning;
  tuning.connect_timeout = 200ms;
  tuning.place_hook = [&](Session& session) {
    if (retire.exchange(false)) session.retire();
  };
  Node a(io, tuning), b(io);
  a.adopt_inbound = false;
  b.adopt_inbound = false;
  retire = true;
  const auto outcome = call(a, b.self);
  EXPECT_FALSE(outcome.reply);
  EXPECT_NE(outcome.failure.find("did not arrive"), std::string::npos) << outcome.failure;
  EXPECT_EQ(a.client->stats().dials, 1u);
}

// In the ordinary race, not forced: the call still comes through, and the pair settles.
TEST(Client, ACallInASimultaneousConnectComesThrough) {
  for (int round = 0; round < 10; ++round) {
    Io io;
    auto first = std::make_unique<Node>(io);
    auto second = std::make_unique<Node>(io);
    auto& low = first->self.id < second->self.id ? *first : *second;
    auto& high = first->self.id < second->self.id ? *second : *first;
    high.adopt_inbound = false;
    ASSERT_TRUE(call(low, high.self).reply);
    ASSERT_FALSE(high.client->has_route(low.self.id, Lane::control));
    auto outcome = std::async(std::launch::async, [&] { return call(high, low.self); });
    high.release_inbound();
    const auto answered = outcome.get();
    EXPECT_TRUE(answered.reply) << answered.failure;
    EXPECT_TRUE(eventually([&] { return high.client->has_route(low.self.id, Lane::control); }));
    EXPECT_TRUE(call(high, low.self).reply);
    EXPECT_TRUE(call(low, high.self).reply);
  }
}

// The journal: Macha's lines at Macha's levels, through the node's sink.
TEST(Journal, EachEndLogsItsConnectionAtInfo) {
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  ClientOptions tuning;
  tuning.log = log;
  Node a(io, tuning), b(io, tuning);
  ASSERT_TRUE(call(a, b.self).reply);
  EXPECT_EQ(log->count(status::LogLevel::info, "node connection outbound peer=" + to_string(b.self.id).substr(0, 12) + " lane=control endpoint=127.0.0.1:"),
            1u);
  EXPECT_TRUE(eventually([&] {
    return log->count(status::LogLevel::info,
                      "node connection inbound peer=" + to_string(a.self.id).substr(0, 12) + " lane=control remote=127.0.0.1 advertised=127.0.0.1:") == 1;
  }));
}

TEST(Journal, ADialFailureIsADebugLine) {
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  ClientOptions tuning;
  tuning.log = log;
  tuning.dial_hook = [](const Endpoint&, Lane) { return std::string("link is down"); };
  Node a(io, tuning), b(io);
  EXPECT_FALSE(call(a, b.self).reply);
  EXPECT_EQ(log->count(status::LogLevel::debug, "node connection failed lane=control"), 1u);
  EXPECT_EQ(log->count(status::LogLevel::debug, "link is down"), 1u);
}

TEST(Journal, ALivenessFailureIsADebugLine) {
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  ClientOptions tuning;
  tuning.log = log;
  tuning.heartbeat = 100ms;
  tuning.dead_after = 600ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  b.hang_pings = 1000;
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, " liveness failure: ") >= 1; }));
}

TEST(Journal, AStallIsNoticedAgainEachPeriodAndACancelSaysSo) {
  Io io;
  test::SteppedTime time;
  auto log = std::make_shared<test::RecordingSink>();
  ClientOptions tuning;
  tuning.log = log;
  tuning.time = &time;
  tuning.heartbeat = 24h;
  tuning.dead_after = 72h;
  tuning.control_stall_notice = 1h;
  tuning.control_no_progress = 3h;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  std::thread stepper([&] {
    b.gate.wait_for_entries(1);
    EXPECT_EQ(log->count(status::LogLevel::debug, "RPC stalled"), 0u);
    time.advance(1h);
    EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "RPC stalled") == 1; }));
    time.advance(1h);
    EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "RPC stalled") == 2; }));
    time.advance(1h);
  });
  const auto stalled = call(a, b.self, slow);
  stepper.join();
  EXPECT_FALSE(stalled.reply);
  EXPECT_EQ(log->count(status::LogLevel::debug, "(control) peer=" + to_string(b.self.id).substr(0, 12) + " message=slow age_ms="), 3u);
  EXPECT_EQ(log->count(status::LogLevel::debug, "RPC made no progress for 10800000 ms"), 1u);
  EXPECT_EQ(log->count(status::LogLevel::debug, "cancelled for retry"), 1u);
}

// With no deadline a stall is only noticed: the call stays and may finish.
TEST(Journal, AStallWithNoDeadlineLeavesTheCallToFinish) {
  Io io;
  test::SteppedTime time;
  auto log = std::make_shared<test::RecordingSink>();
  ClientOptions tuning;
  tuning.log = log;
  tuning.time = &time;
  tuning.heartbeat = 24h;
  tuning.dead_after = 72h;
  tuning.control_stall_notice = 1h;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  std::thread stepper([&] {
    b.gate.wait_for_entries(1);
    time.advance(1h);
    EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "request remains active while peer health is monitored") == 1; }));
    b.gate.open();
  });
  const auto outcome = call(a, b.self, slow);
  stepper.join();
  EXPECT_TRUE(outcome.reply) << outcome.failure;
}

TEST(Journal, StoppingSaysSoAtDebug) {
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  ClientOptions tuning;
  tuning.log = log;
  {
    Node a(io, tuning), b(io);
    ASSERT_TRUE(call(a, b.self).reply);
  }
  for (const auto* line : {"shutdown: client stop begin", "shutdown: client stop complete connections=1", "shutdown: server stop begin",
                           "shutdown: sessions reaping count=", "shutdown: sessions reaped", "shutdown: server stop complete"})
    EXPECT_EQ(log->count(status::LogLevel::debug, line), 1u) << line;
}

TEST(Journal, AFaultingHandlerIsADebugLine) {
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  ClientOptions tuning;
  tuning.log = log;
  Node a(io), b(io, tuning);
  b.throw_on_news = true;
  (void)call(a, b.self, sluggish_throw);
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "RPC handler: the handler broke") == 1; }));
}

// Macha's wording; the once-a-process rule cannot run here, since only Linux
// has a congestion control to choose (and the Debian run exercises the path).
TEST(Journal, TheCongestionControlWarningNamesTheReason) {
  EXPECT_EQ(congestion_control_unavailable(ENOENT), "peer sockets keep the system congestion control: bbr unavailable (No such file or directory)");
}

// A type may have its own no-progress deadline and stall notice.
TEST(Client, ATypesOwnDeadlineReplacesItsLanes) {
  Io io;
  ClientOptions tuning;
  tuning.control_no_progress = 0ms;
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  const auto stalled = call(a, b.self, hurried);
  EXPECT_FALSE(stalled.reply);
  EXPECT_NE(stalled.failure.find("hurried"), std::string::npos) << stalled.failure;
  EXPECT_NE(stalled.failure.find("cancelled for retry"), std::string::npos) << stalled.failure;
}

membership::NodeRecord unreachable(Io& io) {
  membership::NodeRecord nobody;
  nobody.id = identity::NodeId::random();
  nobody.host = "127.0.0.1";
  Node gone(io);
  nobody.port = gone.self.port;
  return nobody;
}

// A failed dial is not retried until its backoff passes.
TEST(Backoff, AFailedDialWaitsOutItsBackoff) {
  Io io;
  Node a(io);
  const auto nobody = unreachable(io);
  EXPECT_NE(call(a, nobody).failure.find("dial"), std::string::npos);
  const auto refused = call(a, nobody);
  EXPECT_NE(refused.failure.find("backoff"), std::string::npos) << refused.failure;
  EXPECT_EQ(a.client->stats().dials, 1u);
  std::this_thread::sleep_for(300ms);
  EXPECT_NE(call(a, nobody).failure.find("dial"), std::string::npos);
  EXPECT_EQ(a.client->stats().dials, 2u);
}

TEST(Backoff, IsPerPeer) {
  Io io;
  Node a(io), b(io);
  EXPECT_FALSE(call(a, unreachable(io)).reply);
  const auto outcome = call(a, b.self);
  EXPECT_TRUE(outcome.reply) << outcome.failure;
}

// Backoff governs new dials, never a route the peer opened meanwhile.
TEST(Backoff, NeverMasksARouteThePeerOpened) {
  Io io;
  Node a(io), c(io);
  auto stale = c.self;
  stale.port = unreachable(io).port;
  EXPECT_FALSE(call(a, stale).reply);
  ASSERT_TRUE(call(c, a.self).reply);
  ASSERT_TRUE(eventually([&] { return a.client->has_route(c.self.id, Lane::control); }));
  const auto outcome = call(a, stale);
  EXPECT_TRUE(outcome.reply) << outcome.failure;
}

// A broadcast reaches every CONTROL route, whichever end dialled it.
TEST(Broadcast, ReachesEveryControlRouteBothWays) {
  Io io;
  Node a(io), b(io), c(io);
  ASSERT_TRUE(call(b, a.self).reply);
  ASSERT_TRUE(call(a, c.self).reply);
  ASSERT_TRUE(eventually([&] { return a.client->has_route(b.self.id, Lane::control); }));
  EXPECT_EQ(a.client->broadcast({news, {1}, {}}), 2u);
  EXPECT_TRUE(eventually([&] { return b.heard == 1 && c.heard == 1; }));
}

// Best effort skips while the route table is busy rather than wait for it;
// broadcast_notify waits for the table but never for a peer.
TEST(Broadcast, BestEffortSkipsABusyRouteTableAndNotifyWaitsForIt) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  size_t skipped = 99;
  std::future<size_t> waited;
  a.client->hold_routes_for_test([&] {
    std::thread([&] { skipped = a.client->broadcast_best_effort({news, {}, {}}, FrameType::speculative); }).join();
    waited = std::async(std::launch::async, [&] { return a.client->broadcast_notify({news, {}, {}}, FrameType::speculative); });
    EXPECT_EQ(waited.wait_for(100ms), std::future_status::timeout);
  });
  EXPECT_EQ(skipped, 0u);
  EXPECT_EQ(waited.get(), 1u);
  EXPECT_EQ(a.client->broadcast_best_effort({news, {}, {}}, FrameType::speculative), 1u);
  EXPECT_TRUE(eventually([&] { return b.heard == 2; }));
}

// Broadcasts go over the routes there are; none dials.
TEST(Broadcast, NeverDials) {
  Io io;
  Node a(io), b(io);
  EXPECT_EQ(a.client->broadcast({news, {}, {}}), 0u);
  EXPECT_EQ(a.client->broadcast_notify({news, {}, {}}, FrameType::control), 0u);
  EXPECT_EQ(a.client->broadcast_best_effort({news, {}, {}}, FrameType::control), 0u);
  EXPECT_EQ(a.client->stats().dials, 0u);
}

constexpr uint32_t incapable = 0;

// A node that accepts no inbound connection is never dialled: with no
// session of its own to ask over, a call to it fails at once.
TEST(ReverseDial, AnIncapableNodeIsNeverDialled) {
  Io io;
  Node site(io, {}, incapable), hub(io);
  const auto outcome = call(hub, site.self);
  EXPECT_FALSE(outcome.reply);
  EXPECT_NE(outcome.failure.find("accepts no inbound connections"), std::string::npos) << outcome.failure;
  EXPECT_EQ(hub.client->stats().dials, 0u);
  EXPECT_EQ(site.server->sessions(), 0u);
}

// CONTROL works both ways over the session the incapable node opened.
TEST(ReverseDial, ControlTravelsBothWaysOverTheIncapableNodesSession) {
  Io io;
  Node site(io, {}, incapable), hub(io);
  ASSERT_TRUE(call(site, hub.self).reply);
  ASSERT_TRUE(eventually([&] { return hub.client->has_route(site.self.id, Lane::control); }));
  const auto outcome = call(hub, site.self);
  EXPECT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(hub.client->stats().dials, 0u);
}

// A DATA lane to an incapable node is asked for over its CONTROL session
// (dial_request); it dials, and the call goes over the lane it opened.
TEST(ReverseDial, ADataLaneIsAskedForAndDialledBack) {
  Io io;
  Node site(io, {}, incapable), hub(io);
  ASSERT_TRUE(call(site, hub.self).reply);
  ASSERT_TRUE(eventually([&] { return hub.client->has_route(site.self.id, Lane::control); }));
  const auto outcome = call(hub, site.self, put_object, FrameType::foreground, Bytes(5000, 3));
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(outcome.reply->payload, Bytes(5000, 3));
  EXPECT_EQ(hub.client->stats().dials, 0u);
  EXPECT_EQ(hub.client->stats().dial_requests_sent, 1u);
  EXPECT_EQ(site.client->stats().dial_requests_received, 1u);
  EXPECT_TRUE(site.client->has_route(hub.self.id, Lane::data));
}

// The wait for a reverse dial is bounded by the connect timeout.
TEST(ReverseDial, AReverseDialThatNeverComesFailsWithinItsBound) {
  Io io;
  ClientOptions tuning;
  tuning.connect_timeout = 300ms;
  Node site(io, {}, incapable), hub(io, tuning);
  site.ignore_dial_requests = true;
  ASSERT_TRUE(call(site, hub.self).reply);
  ASSERT_TRUE(eventually([&] { return hub.client->has_route(site.self.id, Lane::control); }));
  const auto started = std::chrono::steady_clock::now();
  const auto outcome = call(hub, site.self, put_object, FrameType::foreground, Bytes(10, 1));
  const auto took = std::chrono::steady_clock::now() - started;
  EXPECT_FALSE(outcome.reply);
  EXPECT_NE(outcome.failure.find("did not arrive within 300 ms"), std::string::npos) << outcome.failure;
  EXPECT_GE(took, 300ms);
  EXPECT_LT(took, 2s);
}

// An incapable node keeps both lanes dialled to every maintained peer, and
// redials a lane that is lost without being asked.
TEST(ReverseDial, AnIncapableNodeKeepsBothLanesToItsMaintainedPeers) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 100ms;
  Node hub(io), site(io, tuning, incapable);
  site.client->set_maintained_peers([&] { return std::vector<membership::NodeRecord>{hub.self}; });
  EXPECT_TRUE(eventually([&] { return site.client->has_route(hub.self.id, Lane::control) && site.client->has_route(hub.self.id, Lane::data); }));
  EXPECT_TRUE(eventually([&] { return hub.client->has_route(site.self.id, Lane::data); }));
  site.client->close_route(hub.self.id, Lane::data, "severed by the test");
  EXPECT_FALSE(site.client->has_route(hub.self.id, Lane::data));
  EXPECT_TRUE(eventually([&] { return site.client->has_route(hub.self.id, Lane::data); }));
  EXPECT_GE(site.client->stats().dials, 3u);
}

// A capable node needs no maintenance: peers reach it.
TEST(ReverseDial, ACapableNodeMaintainsNothing) {
  Io io;
  ClientOptions tuning;
  tuning.heartbeat = 50ms;
  Node hub(io), other(io, tuning);
  other.client->set_maintained_peers([&] { return std::vector<membership::NodeRecord>{hub.self}; });
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(other.client->stats().dials, 0u);
}

// The node is told of each session this client dials, with the peer's
// record, as of each session a peer opens: both are direct observations.
TEST(Client, TellsTheNodeOfEachSessionItDials) {
  Io io;
  std::mutex mutex;
  std::vector<identity::NodeId> seen;
  ClientOptions tuning;
  tuning.established = [&](const Established& session) {
    std::lock_guard lock(mutex);
    seen.push_back(session.peer.id);
  };
  Node a(io, tuning), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  ASSERT_TRUE(call(a, b.self, put_object, FrameType::foreground, Bytes(10, 1)).reply);
  std::lock_guard lock(mutex);
  EXPECT_EQ(seen, (std::vector<identity::NodeId>{b.self.id, b.self.id}));
}

// An identity reset closes the routes it covers and forgets the address's
// node, so the next call dials afresh and checks who answers.
TEST(Client, AnIdentityResetClosesTheRoutesItCovers) {
  Io io;
  Node a(io), b(io);
  ASSERT_TRUE(call(a, b.self).reply);
  membership::IdentityReset reset;
  reset.host = b.self.host;
  reset.port = b.self.port;
  reset.stale_node_id = b.self.id;
  reset.epoch = 1;
  a.client->invalidate_identity_association(reset);
  EXPECT_FALSE(a.client->has_route(b.self.id, Lane::control));
  ASSERT_TRUE(call(a, b.self).reply);
  EXPECT_EQ(a.client->stats().dials, 2u);
}

// A call's handle: cancelled before or after admission it completes at
// once as cancelled; promoted it travels at the new class; idle_for says how
// long the reply has been coming; aborted, the route goes with it.
TEST(Client, ACallHandleCancelsPromotesAndAborts) {
  Io io;
  Node a(io), b(io);
  auto done = std::make_shared<std::promise<CallOutcome>>();
  auto handle = a.client->call(b.self, slow, {}, FrameType::control, io.context.get_executor(), [done](CallOutcome o) { done->set_value(std::move(o)); });
  ASSERT_TRUE(b.gate.wait_for_entries(1));
  std::this_thread::sleep_for(50ms);
  EXPECT_GE(handle->idle_for(), 40ms);
  handle->promote(FrameType::foreground);
  handle->cancel();
  auto future = done->get_future();
  ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
  EXPECT_NE(future.get().failure.find("cancelled"), std::string::npos);
  EXPECT_EQ(handle->idle_for(), std::chrono::milliseconds::max());
  b.gate.open();
  // Cancelled before the dial completes: never placed.
  Node c(io);
  auto early = std::make_shared<std::promise<CallOutcome>>();
  auto pending =
      a.client->call(c.self, message::ping, {}, FrameType::control, io.context.get_executor(), [early](CallOutcome o) { early->set_value(std::move(o)); });
  pending->cancel();
  auto early_future = early->get_future();
  ASSERT_EQ(early_future.wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(early_future.get().reply);
  // Aborted: the call fails and the session it was on is closed.
  ASSERT_TRUE(call(a, b.self).reply);
  auto aborted = std::make_shared<std::promise<CallOutcome>>();
  b.hang_pings = 0;
  auto to_abort =
      a.client->call(b.self, slow, {}, FrameType::control, io.context.get_executor(), [aborted](CallOutcome o) { aborted->set_value(std::move(o)); });
  ASSERT_TRUE(b.gate.wait_for_entries(2));
  to_abort->abort();
  ASSERT_EQ(aborted->get_future().wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(eventually([&] { return !a.client->has_route(b.self.id, Lane::control); }));
}

// A test's seam: a dial refused before it is made, as if the network were
// down, earns the backoff a failed dial does.
TEST(Client, ADialHookRefusesADial) {
  Io io;
  ClientOptions tuning;
  tuning.dial_hook = [](const Endpoint& endpoint, Lane) { return endpoint.port % 2 ? std::string("link down") : std::string(); };
  Node a(io, tuning), b(io);
  const auto outcome = call(a, b.self);
  if (b.self.port % 2) {
    EXPECT_FALSE(outcome.reply);
    EXPECT_NE(outcome.failure.find("link down"), std::string::npos) << outcome.failure;
    EXPECT_EQ(a.client->stats().dials, 1u);
    EXPECT_NE(call(a, b.self).failure.find("backoff"), std::string::npos);
  } else {
    EXPECT_TRUE(outcome.reply) << outcome.failure;
  }
}

}  // namespace
}  // namespace dannan::net
