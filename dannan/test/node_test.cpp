//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Dannan's node and dannan::Context: step 5's gate. A Dannan-only cluster
// in one process (membership both ways, telemetry, identity resets, the
// inbound resolution by dial-back, readiness), and a hosted system that is
// nothing but a counter running on it through the Context.

#include "dannan/node.h"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <fstream>
#include <future>
#include <thread>

#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using sched::FrameType;

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

identity::ClusterKey cluster_key(const std::filesystem::path& dir) {
  std::ofstream(dir / "cluster.key") << "node test";
  return identity::load_cluster_key(dir / "cluster.key");
}

NodeConfig config(const std::filesystem::path& state, const identity::ClusterKey& key, std::vector<net::Endpoint> bootstrap = {}) {
  NodeConfig out;
  out.state_path = state;
  out.key = key;
  out.listen_host = "127.0.0.1";
  out.advertise_host = "127.0.0.1";
  out.port = 0;
  out.bootstrap = std::move(bootstrap);
  out.inbound = reach::Inbound::yes;
  out.heartbeat = 100ms;
  out.dead_after = 2s;
  out.telemetry_interval = 100ms;
  out.connect_timeout = 1s;
  out.version = "0.1.0-test";
  return out;
}

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds bound = 10s) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return condition();
}

bool knows(const Node& node, const identity::NodeId& id) {
  const auto roster = node.context().nodes().active();
  return std::any_of(roster.begin(), roster.end(), [&](const membership::NodeRecord& n) { return n.id == id; });
}

struct Pair {
  Io io;
  test::TempDir dir;
  identity::ClusterKey key = cluster_key(dir.path());
  std::unique_ptr<Node> a;
  std::unique_ptr<Node> b;
  // `setup_a` runs before A starts: a hosted system registers its routes then.
  explicit Pair(const std::function<void(NodeConfig&)>& tune_b = {}, const std::function<void(Node&)>& setup_a = {}) {
    a = std::make_unique<Node>(io.context, config(dir.path() / "a", key));
    if (setup_a) setup_a(*a);
    a->start();
    auto b_config = config(dir.path() / "b", key, {{"127.0.0.1", a->port()}});
    if (tune_b) tune_b(b_config);
    b = std::make_unique<Node>(io.context, b_config);
  }
  ~Pair() {
    if (b) b->stop();
    if (a) a->stop();
  }
};

// A founding node and one that bootstraps from it: each lists the other,
// directly, and each has the other's telemetry.
TEST(Node, TwoNodesFormOneCluster) {
  Pair pair;
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  const auto b = pair.b->context().identity().id();
  EXPECT_TRUE(eventually([&] { return knows(*pair.a, b) && knows(*pair.b, a); }));
  EXPECT_TRUE(pair.a->context().nodes().directly_reachable(b));
  EXPECT_TRUE(eventually([&] {
    const auto all = pair.a->context().telemetry().all();
    return std::any_of(all.begin(), all.end(), [&](const status::Telemetry& t) { return t.node_id() == b && t.text(status::field::version) == "0.1.0-test"; });
  }));
  EXPECT_TRUE(eventually([&] { return pair.b->context().nodes().latency(a).has_value(); }));
}

// A hosted system that is nothing but a counter: a user type registered
// through the Context, called across the cluster, its count in Status.
TEST(Node, ACounterRunsOnIt) {
  constexpr net::MessageType add{1300};
  Pair pair({}, [&](Node& a) {
    std::atomic<uint64_t>* total = &a.context().status().counter("counter.total");
    a.context().routes().add(add, {"counter_add", net::Lanes::control, net::Frames::control_only, false},
                             [total](const membership::NodeRecord&, FrameType, const net::Message& request) {
                               net::Reader reader(request.payload);
                               const auto amount = reader.u64();
                               total->fetch_add(amount);
                               net::Writer writer;
                               writer.u64(total->load());
                               return net::Handled::processed({net::message::ok, writer.take(), {}});
                             });
    // A core type is not a hosted system's to take.
    EXPECT_THROW(a.context().routes().add(net::message::ping, {"mine", net::Lanes::control, net::Frames::any, false}, {}), std::logic_error);
  });
  pair.b->context().routes().add(add, {"counter_add", net::Lanes::control, net::Frames::control_only, false},
                                 [](const membership::NodeRecord&, FrameType, const net::Message&) { return net::Handled::unrecognised(); });
  pair.b->start();
  const auto a = pair.a->context().nodes().self();
  for (uint64_t i = 1; i <= 5; ++i) {
    std::promise<net::CallOutcome> done;
    net::Writer amount;
    amount.u64(i);
    pair.b->context().routes().call(a, add, amount.take(), FrameType::control, 5s, pair.io.context.get_executor(),
                                    [&](net::CallOutcome outcome) { done.set_value(std::move(outcome)); });
    const auto outcome = done.get_future().get();
    ASSERT_TRUE(outcome.reply) << outcome.failure;
  }
  EXPECT_EQ(pair.a->context().status().snapshot().counters.at("counter.total"), 15u);
  // Routes are fixed once a node has started.
  EXPECT_THROW(pair.a->context().routes().add(net::MessageType{1302}, {"late", net::Lanes::control, net::Frames::any, false}, {}), std::logic_error);
}

// A call never waits past its deadline.
TEST(Node, ACallEndsAtItsDeadline) {
  constexpr net::MessageType slow{1301};
  std::promise<void> release;
  auto released = release.get_future().share();
  Pair pair({}, [&](Node& a) {
    a.context().routes().add(slow, {"slow", net::Lanes::control, net::Frames::control_only, false},
                             [released](const membership::NodeRecord&, FrameType, const net::Message&) {
                               released.wait();
                               return net::Handled::processed();
                             });
  });
  pair.b->start();
  std::promise<net::CallOutcome> done;
  const auto started = std::chrono::steady_clock::now();
  pair.b->context().routes().call(pair.a->context().nodes().self(), slow, {}, FrameType::control, 300ms, pair.io.context.get_executor(),
                                  [&](net::CallOutcome outcome) { done.set_value(std::move(outcome)); });
  const auto outcome = done.get_future().get();
  EXPECT_FALSE(outcome.reply);
  EXPECT_NE(outcome.failure.find("deadline"), std::string::npos) << outcome.failure;
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
  release.set_value();
}

// Telemetry's phase follows readiness: recovering until every declared
// plane is ready.
TEST(Node, ThePhaseFollowsTheDeclaredPlanes) {
  Pair pair([](NodeConfig& c) { c.planes = {"metadata"}; });
  pair.b->start();
  EXPECT_EQ(pair.b->readiness().phase(), status::Phase::recovering);
  EXPECT_TRUE(eventually([&] { return pair.b->context().telemetry().local() && pair.b->context().telemetry().local()->phase() == status::Phase::recovering; }));
  pair.b->readiness().ready("metadata");
  EXPECT_EQ(pair.b->readiness().phase(), status::Phase::ready);
  EXPECT_TRUE(eventually([&] { return pair.b->context().telemetry().local()->phase() == status::Phase::ready; }));
}

// A reset made on one node reaches its peers.
TEST(Node, AnIdentityResetPropagates) {
  Pair pair;
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return knows(*pair.a, pair.b->context().identity().id()); }));
  membership::IdentityReset reset;
  reset.host = "10.9.9.9";
  reset.port = 7437;
  reset.epoch = 1;
  reset.reset_unix_ms = 1;
  reset.reason = "disk replaced";
  pair.a->propagate_identity_reset(reset, 5s);
  EXPECT_TRUE(eventually([&] { return !pair.b->membership().identity_resets().empty(); }));
}

// `automatic`: a peer dials back and the node learns it is capable.
TEST(Node, AnAutomaticNodeIsResolvedByDialBack) {
  Pair pair([](NodeConfig& c) { c.inbound = reach::Inbound::automatic; });
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return pair.b->inbound().state().source.starts_with("probe:"); }));
  EXPECT_TRUE(pair.b->inbound().capable());
}

// A declared unit of work the ledger cannot hold is refused at start.
TEST(Node, ABudgetBelowOneUnitIsRefused) {
  Pair pair;
  auto& budget = pair.a->context().budget();
  EXPECT_NO_THROW(budget.declare(sched::WorkClass::foreground, 1 << 20));
  EXPECT_THROW(budget.declare(sched::WorkClass::foreground, 1ull << 40), std::invalid_argument);
  auto lease = budget.reserve(sched::WorkClass::foreground, 4096);
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease->bytes(), 4096u);
}

TEST(Node, StoppingIsPromptAndCallsFailAfter) {
  Pair pair;
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return knows(*pair.b, pair.a->context().identity().id()); }));
  const auto started = std::chrono::steady_clock::now();
  pair.b->stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
  std::promise<net::CallOutcome> done;
  pair.b->context().routes().call(pair.a->context().nodes().self(), net::message::members, {}, FrameType::control, 1s, pair.io.context.get_executor(),
                                  [&](net::CallOutcome outcome) { done.set_value(std::move(outcome)); });
  EXPECT_FALSE(done.get_future().get().reply);
}

// A hosted system logs through the node's one sink, under its own scope.
TEST(Node, AHostedSystemLogsThroughTheNodesSink) {
  struct Recording final : status::LogSink {
    std::mutex mutex;
    std::vector<std::string> lines;
    bool enabled(status::LogLevel level) const noexcept override { return level >= status::LogLevel::info; }
    void write(status::LogLevel, std::string_view scope, std::string_view message) override {
      std::lock_guard lock(mutex);
      lines.push_back(std::string(scope) + ": " + std::string(message));
    }
  };
  auto sink = std::make_shared<Recording>();
  Io io;
  test::TempDir dir;
  auto node_config = config(dir.path() / "a", cluster_key(dir.path()));
  node_config.log = sink;
  Node node(io.context, node_config);
  const auto logger = node.context().logger("lugh");
  logger.info("signed in");
  logger.child("oauth").warn("token refused");
  logger.debug("not written");
  EXPECT_EQ(sink->lines, (std::vector<std::string>{"lugh: signed in", "lugh.oauth: token refused"}));
}

TEST(Node, ReportsItsContextVersion) {
  Pair pair;
  EXPECT_EQ(pair.a->context().version(), context_version);
}

constexpr net::MessageType echo_type{1310};
constexpr net::MessageType news_type{1311};

// Routes are bound and unbound while the node runs, as a subsystem starts
// and stops; an unbound type answers 501; unbinding waits for calls in
// flight on it.
TEST(Node, RoutesBindAndUnbindWhileRunning) {
  Pair pair({}, [](Node& a) { a.context().routes().add(echo_type, {"echo", net::Lanes::control, net::Frames::control_only, false}); });
  pair.b->start();
  const auto a = pair.a->context().nodes().self();
  auto outcome = pair.b->context().routes().call_and_wait(a, echo_type, {1}, FrameType::control, 5s);
  ASSERT_TRUE(outcome.reply);
  EXPECT_EQ(net::refusal_of(*outcome.reply)->status, 501);
  test::Gate gate;
  test::GateOpener opener(gate);
  pair.a->context().routes().bind(echo_type, [&](const membership::NodeRecord&, FrameType, const net::Message& request) {
    gate.enter_and_wait();
    return net::Handled::processed({net::message::ok, request.payload, {}});
  });
  auto done = std::make_shared<std::promise<net::CallOutcome>>();
  pair.b->context().routes().call(a, echo_type, {2}, FrameType::control, 5s, pair.io.context.get_executor(),
                                  [done](net::CallOutcome o) { done->set_value(std::move(o)); });
  ASSERT_TRUE(gate.wait_for_entries(1));
  std::atomic<bool> unbound{false};
  std::thread unbinder([&] {
    pair.a->context().routes().unbind(echo_type);
    unbound = true;
  });
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(unbound.load());
  gate.open();
  unbinder.join();
  EXPECT_EQ(done->get_future().get().reply->payload, net::Bytes{2});
  outcome = pair.b->context().routes().call_and_wait(a, echo_type, {3}, FrameType::control, 5s);
  EXPECT_EQ(net::refusal_of(*outcome.reply)->status, 501);
  EXPECT_EQ(outcome.peer.id, a.id);
}

// A hosted system's adverts ride in its record: set through the Context,
// they reach every peer's roster by gossip, a change raises a topology
// event there, and placement can be narrowed to the nodes carrying one and
// weighed by another.
TEST(Node, AdvertsReachPeersAndSteerPlacement) {
  Pair pair;
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  const auto b = pair.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.a, b) && knows(*pair.b, a); }));
  const auto topology = pair.b->context().events().count(status::NodeEvents::topology);
  pair.a->context().nodes().advertise("capacity", membership::u64_advert(4096));
  const auto advertised = [&] {
    for (const auto& node : pair.b->context().nodes().active())
      if (node.id == a) return node.advert_u64("capacity") == 4096u;
    return false;
  };
  EXPECT_TRUE(eventually(advertised));
  EXPECT_TRUE(eventually([&] { return pair.b->context().events().count(status::NodeEvents::topology) > topology; }));
  const placement::Scheme scheme{"test/fallback", "test/extra-domain", "capacity"};
  const std::vector<uint8_t> key(32, 7);
  const auto placed = pair.b->context().nodes().placement(key, 1, scheme, "capacity");
  ASSERT_EQ(placed.size(), 1u);
  EXPECT_EQ(placed.front().id, a);
  EXPECT_EQ(pair.b->context().nodes().placement(key, 2, scheme).size(), 2u);
  EXPECT_THROW(pair.b->context().nodes().placement(key, 1, placement::Scheme{}), std::invalid_argument);
  pair.a->context().nodes().withdraw("capacity");
  EXPECT_TRUE(eventually([&] { return pair.b->context().nodes().placement(key, 1, scheme, "capacity").empty(); }));
  EXPECT_THROW(pair.a->context().nodes().advertise("", {}), std::invalid_argument);
}

// Channels across a real cluster: B subscribes, A learns of it by gossip
// and publishes only there; notify and stream arrive, a stream in order and
// numbered, and bulk is acknowledged.
TEST(Node, ChannelsReachSubscribersAcrossTheCluster) {
  Pair pair;
  pair.b->start();
  const auto b = pair.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.a, b) && knows(*pair.b, pair.a->context().identity().id()); }));
  for (auto* node : {pair.a.get(), pair.b.get()}) {
    node->context().channels().open("news", net::Delivery::notify);
    node->context().channels().open("seq", net::Delivery::stream);
    node->context().channels().open("bulk", net::Delivery::bulk);
  }
  std::mutex mutex;
  std::vector<net::ChannelMessage> got;
  const auto strand = asio::make_strand(pair.io.context);
  const auto record = [&](net::ChannelMessage m) {
    std::lock_guard lock(mutex);
    got.push_back(std::move(m));
  };
  for (const auto* channel : {"news", "seq", "bulk"}) pair.b->context().channels().subscribe(channel, "room/", strand, record);
  // A sees B's subscription by gossip.
  ASSERT_TRUE(eventually([&] {
    for (const auto& node : pair.a->context().nodes().active())
      if (node.id == b && node.advert("dannan.ch.seq")) return true;
    return false;
  }));
  auto& channels = pair.a->context().channels();
  EXPECT_EQ(channels.publish("news", "room/1", net::Bytes{1}).to, std::vector<identity::NodeId>{b});
  EXPECT_TRUE(channels.publish("news", "hall/1", net::Bytes{2}).to.empty());
  for (uint8_t i = 1; i <= 3; ++i) channels.publish("seq", "room/1", net::Bytes{i});
  std::promise<net::Published> bulk;
  channels.publish_bulk("bulk", "room/2", net::Bytes(4096, 9), 5s, pair.io.context.get_executor(), [&](net::Published p) { bulk.set_value(std::move(p)); });
  EXPECT_EQ(bulk.get_future().get().to, std::vector<identity::NodeId>{b});
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(mutex);
    return got.size() == 5;
  }));
  std::lock_guard lock(mutex);
  std::vector<uint64_t> sequence;
  for (const auto& m : got) {
    EXPECT_EQ(m.origin, pair.a->context().identity().id());
    EXPECT_EQ(m.incarnation, pair.a->context().identity().durability_epoch());
    if (m.channel == "seq") sequence.push_back(m.sequence);
  }
  EXPECT_EQ(sequence, (std::vector<uint64_t>{1, 2, 3}));
}

// The three broadcasts reach every CONTROL route; none dials.
TEST(Node, TheThreeBroadcastsReachPeers) {
  std::atomic<int> heard{0};
  Pair pair({}, [&](Node& a) {
    a.context().routes().add(news_type, {"news", net::Lanes::control, net::Frames::any, true},
                             [&](const membership::NodeRecord&, FrameType, const net::Message&) {
                               ++heard;
                               return net::Handled::processed();
                             });
  });
  pair.b->start();
  const auto a_id = pair.a->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.b, a_id) && pair.b->client_stats()->reused > 0; }));
  auto& channels = pair.b->context().channels();
  EXPECT_EQ(channels.broadcast({news_type, {}, {}}), 1u);
  EXPECT_EQ(channels.notify({news_type, {}, {}}, FrameType::speculative), 1u);
  EXPECT_TRUE(eventually([&] { return channels.broadcast_best_effort({news_type, {}, {}}, FrameType::speculative) == 1; }));
  EXPECT_TRUE(eventually([&] { return heard.load() == 3; }));
}

// What Macha's Status page and its facade read, through the Context.
TEST(Node, ExposesTelemetryReadinessDiagnosticsActivityAndObservations) {
  Pair pair;
  pair.b->start();
  const auto a_id = pair.a->context().identity().id();
  const auto b_id = pair.b->context().identity().id();
  EXPECT_TRUE(eventually([&] { return knows(*pair.a, b_id) && knows(*pair.b, a_id); }));
  auto& telemetry = pair.a->context().telemetry();
  EXPECT_TRUE(eventually([&] {
    const auto views = telemetry.views(5s);
    return std::any_of(views.begin(), views.end(), [&](const status::TelemetryView& v) { return v.telemetry.node_id() == b_id && v.fresh; });
  }));
  telemetry.persist();
  EXPECT_FALSE(telemetry.persisted().empty());
  const auto snapshot = pair.a->context().nodes().snapshot();
  EXPECT_EQ(snapshot.all.size(), 2u);
  EXPECT_EQ(snapshot.active.size(), 2u);
  EXPECT_TRUE(pair.a->context().nodes().all_known_reachable());
  EXPECT_TRUE(eventually([&] { return !pair.a->context().nodes().latencies().empty(); }));
  EXPECT_TRUE(pair.a->context().nodes().identity_resets().empty());
  const auto diagnostics = pair.a->context().diagnostics();
  EXPECT_GT(diagnostics.started_unix_ms, 0u);
  EXPECT_EQ(diagnostics.inbound.source, "configured");
  EXPECT_GT(diagnostics.traffic.in_bytes[1] + diagnostics.traffic.out_bytes[1], 0u);
  EXPECT_GE(diagnostics.work.message_timings.size(), 1u);
  EXPECT_FALSE(diagnostics.threads.empty());
  EXPECT_TRUE(pair.a->context().readiness().control_plane_online());
  EXPECT_LT(pair.a->context().activity().idle_for(sched::WorkClass::control), 1h);
  pair.a->context().observations().add("fs.requests", 2);
  EXPECT_EQ(pair.a->context().observations().snapshot().counters.at("fs.requests"), 2u);
}

// A worker blocked in a call is released before it is joined: outbound
// calls are cancelled first, the rest of the node after.
TEST(Node, CancellingOutboundCallsReleasesAWaitingWorker) {
  constexpr net::MessageType slow{1312};
  std::promise<void> release;
  auto released = release.get_future().share();
  Pair pair({}, [&](Node& a) {
    a.context().routes().add(slow, {"slow", net::Lanes::control, net::Frames::control_only, false},
                             [released](const membership::NodeRecord&, FrameType, const net::Message&) {
                               released.wait();
                               return net::Handled::processed();
                             });
  });
  pair.b->start();
  std::thread worker([&] { (void)pair.b->context().routes().call_and_wait(pair.a->context().nodes().self(), slow, {}, FrameType::control, 30s); });
  std::this_thread::sleep_for(200ms);
  pair.b->cancel_outbound_calls();
  const auto started = std::chrono::steady_clock::now();
  worker.join();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
  release.set_value();
}

}  // namespace
}  // namespace dannan
