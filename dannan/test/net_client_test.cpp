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

constexpr MessageType put_object{2005};
constexpr MessageType slow{1200};
constexpr MessageType sluggish{1201};
constexpr MessageType news{1202};

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
    server_options.key = key;
    server_options.local = [this] { return self; };
    server_options.routes = table();
    server_options.traffic = traffic;
    server = std::make_unique<Server>(
        io.context, "127.0.0.1", 0, server_options,
        [this](const membership::NodeRecord& peer, FrameType, const Message& request) {
          if (request.type == message::dial_request) return ignore_dial_requests ? Handled::processed() : client->handle_dial_request(peer, request);
          if (request.type == slow) gate.enter_and_wait();
          if (request.type == sluggish) std::this_thread::sleep_for(300ms);
          if (request.type == news) ++heard;
          if (request.type == message::ping && hang_pings.fetch_sub(1) > 0) gate.enter_and_wait();
          return Handled::processed({message::ok, request.payload, {}});
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
  tuning.established = [&](const membership::NodeRecord& peer) {
    std::lock_guard lock(mutex);
    seen.push_back(peer.id);
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
