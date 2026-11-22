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

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/ports.h"
#include "dannan/test/fixtures/recording_sink.h"
#include "dannan/test/fixtures/stepped_time.h"
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
  // `setup_a` runs before A starts: a hosted system registers its routes then. With
  // `genesis_on_a`, A alone is the agreement genesis set of both.
  explicit Pair(const std::function<void(NodeConfig&)>& tune_b = {}, const std::function<void(Node&)>& setup_a = {}, bool genesis_on_a = false) {
    auto a_config = config(dir.path() / "a", key);
    if (genesis_on_a) {
      // A's id is made with its state; a node built and dropped unstarted leaves it there.
      a_config.agreement_genesis = {Node(io.context, a_config).context().identity().id()};
    }
    a = std::make_unique<Node>(io.context, a_config);
    if (setup_a) setup_a(*a);
    a->start();
    auto b_config = config(dir.path() / "b", key, {{"127.0.0.1", a->port()}});
    b_config.agreement_genesis = a_config.agreement_genesis;
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

// Large transfers go round the node's pool (0.11.0): a request's fragments and joined payload
// come from it and go back once the handler returns, and a reply goes back once written, so a
// second transfer of the size reuses what the first took.
TEST(Node, LargeTransfersGoRoundTheNodesBufferPool) {
  constexpr net::MessageType echo{1100};
  Pair pair({}, [&](Node& a) {
    a.context().routes().add(
        echo, {"echo", net::Lanes::data, net::Frames::not_control, false},
        [](const NodeRecord&, sched::FrameType, const net::Message& request) { return net::Handled::processed({net::message::ok, request.payload, {}}); });
  });
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.b, a); }));
  const auto peer = [&] {
    for (const auto& node : pair.b->context().nodes().active())
      if (node.id == a) return node;
    return NodeRecord{};
  }();
  const net::Bytes payload(4 * 1024 * 1024, 0x42);
  for (int i = 0; i < 2; ++i) {
    const auto outcome = pair.b->context().routes().call_and_wait(peer, echo, payload, sched::FrameType::foreground, 10s);
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->payload, payload);
  }
  const auto served = pair.a->context().buffers()->stats();
  EXPECT_GT(served.kept, 0u);
  EXPECT_GT(served.reused, 0u);
  EXPECT_LE(served.held_bytes, NodeConfig{}.transfer_buffers_bound);
}

// A node with another agreement slot table (another count, or another genesis set) is refused
// when it joins (the maintainer, 2026-11-16): the node it dials refuses the handshake and says why in its
// journal, and neither node lists the other. A matching node joins (TwoNodesFormOneCluster).
TEST(Node, ANodeWithAnotherSlotTableIsRefusedAtJoin) {
  const std::vector<std::function<void(NodeConfig&)>> others = {
      [](NodeConfig& config) { config.agreement_slots = 512; },
      [](NodeConfig& config) { config.agreement_genesis = {identity::NodeId::random()}; },
  };
  for (const auto& other : others) {
    Io io;
    test::TempDir dir;
    const auto key = cluster_key(dir.path());
    auto log = std::make_shared<test::RecordingSink>();
    auto a_config = config(dir.path() / "a", key);
    a_config.log = log;
    Node a(io.context, a_config);
    a.start();
    auto b_config = config(dir.path() / "b", key, {{"127.0.0.1", a.port()}});
    other(b_config);
    Node b(io.context, b_config);
    b.start();
    // B's dial is refused by A, which says why; B sees only the session end.
    const auto refused = b.context().routes().call_and_wait(net::Endpoint{"127.0.0.1", a.port()}, net::message::telemetry, {}, sched::FrameType::control, 5s);
    EXPECT_FALSE(refused.reply);
    EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "slot table") > 0; }));
    EXPECT_FALSE(knows(a, b.context().identity().id()));
    EXPECT_FALSE(knows(b, a.context().identity().id()));
    b.stop();
    a.stop();
  }
}

// Lir's TSan report against 0.8.3 (run 43773090610): nodes joining while adverts change and
// dials run, in one process on one io_context of four threads. A dial's handshake carries a
// copy of its node's record and the server observes the peer's; neither may share anything
// with the other or with the roster. Every join completes, and under TSan nothing races.
TEST(Node, NodesJoinWhileAdvertsChangeAndNothingIsShared) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  Node a(io.context, config(dir.path() / "a", key));
  a.start();
  std::atomic<bool> churning{true};
  // The node joining this round, held under the mutex while the churn uses it.
  std::mutex joining_mutex;
  Node* joining = nullptr;
  // Adverts change on both sides throughout, as a hosted system's subscriptions do.
  std::thread churn([&] {
    for (uint64_t i = 0; churning; ++i) {
      const auto name = "interest/" + std::to_string(i % 7);
      const net::Bytes value(1 + i % 13, static_cast<uint8_t>(i));
      a.context().nodes().advertise(name, value);
      if (i % 3 == 0) a.context().nodes().withdraw(name);
      std::lock_guard lock(joining_mutex);
      if (!joining) continue;
      joining->context().nodes().advertise(name, value);
      if (i % 3 == 0) joining->context().nodes().withdraw(name);
      std::this_thread::yield();
    }
  });
  for (int round = 0; round < 12; ++round) {
    auto b = std::make_unique<Node>(io.context, config(dir.path() / ("b" + std::to_string(round)), key, {{"127.0.0.1", a.port()}}));
    b->start();
    {
      std::lock_guard lock(joining_mutex);
      joining = b.get();
    }
    const auto id = b->context().identity().id();
    EXPECT_TRUE(eventually([&] { return knows(a, id) && knows(*b, a.context().identity().id()); })) << "round " << round;
    {
      std::lock_guard lock(joining_mutex);
      joining = nullptr;
    }
    b->stop();
  }
  churning = false;
  churn.join();
  a.stop();
}

// Two nodes whose gossip is slow (a 30 s heartbeat), so only a push carries an advert
// within a test: B bootstraps from A, and each learns the other in the handshake.
struct SlowGossip {
  Io io;
  test::TempDir dir;
  identity::ClusterKey key = cluster_key(dir.path());
  std::unique_ptr<Node> a;
  std::unique_ptr<Node> b;
  SlowGossip() {
    auto a_config = config(dir.path() / "a", key);
    a_config.heartbeat = 30s;
    a_config.dead_after = 90s;
    a = std::make_unique<Node>(io.context, a_config);
    a->start();
    auto b_config = config(dir.path() / "b", key, {{"127.0.0.1", a->port()}});
    b_config.heartbeat = 30s;
    b_config.dead_after = 90s;
    b = std::make_unique<Node>(io.context, b_config);
    b->start();
  }
  ~SlowGossip() {
    if (b) b->stop();
    if (a) a->stop();
  }
  // `of`'s advert `key` as `at` holds it.
  static std::optional<net::Bytes> advert_at(Node& at, const identity::NodeId& of, const std::string& key) {
    for (const auto& record : at.context().nodes().active())
      if (record.id == of)
        if (const auto value = record.advert(key)) return net::Bytes(value->begin(), value->end());
    return std::nullopt;
  }
};

membership::Pushed pushed_now(const std::function<void(std::function<void(membership::Pushed)>)>& start) {
  auto told = std::make_shared<std::promise<membership::Pushed>>();
  start([told](membership::Pushed pushed) { told->set_value(std::move(pushed)); });
  auto future = told->get_future();
  if (future.wait_for(30s) != std::future_status::ready) return {};
  return future.get();
}

// Nodes::advertise with a deadline (the maintainer, 2026-11-16, for Lir): the changed record goes to
// every active peer at once and each merges it before answering, so when `done` says a
// peer holds it, it does; a withdrawal the same; a peer that is gone is named as missed.
TEST(Node, APushedAdvertIsHeldByEveryPeerBeforeItIsDone) {
  SlowGossip cluster;
  const auto a = cluster.a->context().identity().id();
  const auto b = cluster.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*cluster.a, b) && knows(*cluster.b, a); }));
  const net::Bytes value{1, 2, 3};
  const auto set = pushed_now([&](auto done) { cluster.a->context().nodes().advertise("lir.interest", value, 5s, cluster.io.context.get_executor(), done); });
  EXPECT_EQ(set.held, std::vector<identity::NodeId>{b});
  EXPECT_TRUE(set.missed.empty());
  EXPECT_EQ(SlowGossip::advert_at(*cluster.b, a, "lir.interest"), value);

  const auto cleared =
      pushed_now([&](auto done) { cluster.a->context().nodes().advertise("lir.interest", std::nullopt, 5s, cluster.io.context.get_executor(), done); });
  EXPECT_EQ(cleared.held, std::vector<identity::NodeId>{b});
  EXPECT_FALSE(SlowGossip::advert_at(*cluster.b, a, "lir.interest"));

  cluster.b->stop();
  const auto gone = pushed_now([&](auto done) { cluster.a->context().nodes().advertise("lir.interest", value, 2s, cluster.io.context.get_executor(), done); });
  EXPECT_TRUE(gone.held.empty());
  ASSERT_EQ(gone.missed.size(), 1u);
  EXPECT_EQ(gone.missed[0].first, b);
  EXPECT_FALSE(gone.missed[0].second.empty());
}

// Lir's window (2026-11-16): a subscription's interest travelled at the heartbeat's pace, so
// a publish just after a SUBACK went nowhere. Subscribed with a deadline, the interest is
// held by the publisher before `done`, and the very next publish reaches the subscriber.
TEST(Node, ASubscriptionHeldBeforeItsDoneGetsTheNextPublish) {
  SlowGossip cluster;
  const auto a = cluster.a->context().identity().id();
  const auto b = cluster.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*cluster.a, b) && knows(*cluster.b, a); }));
  cluster.a->context().channels().open("events", net::Delivery::stream);
  cluster.b->context().channels().open("events", net::Delivery::stream);
  auto received = std::make_shared<std::promise<std::string>>();
  const auto subscribed = pushed_now([&](auto done) {
    (void)cluster.b->context().channels().subscribe(
        "events", "sensors/", cluster.io.context.get_executor(),
        [received](const net::ChannelMessage& message) { received->set_value(std::string(message.payload.begin(), message.payload.end())); }, 5s,
        cluster.io.context.get_executor(), done);
  });
  EXPECT_EQ(subscribed.held, std::vector<identity::NodeId>{a});
  const auto published = cluster.a->context().channels().publish("events", "sensors/1", net::Bytes{'h', 'i'});
  EXPECT_NE(std::find(published.to.begin(), published.to.end(), b), published.to.end());
  auto message = received->get_future();
  ASSERT_EQ(message.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(message.get(), "hi");
}

// A node stopped while its io ran may be destroyed after the io has stopped (Louhi,
// 2026-11-16): its client's second stop, from the destructor, has nothing left to wait for,
// and must not post to an io that will never run it.
TEST(Node, ANodeStoppedThenDestroyedAfterItsIoStopsDoesNotHang) {
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  asio::io_context context;
  auto work = asio::make_work_guard(context);
  std::vector<std::thread> threads;
  for (int i = 0; i < 2; ++i) threads.emplace_back([&] { context.run(); });
  auto node = std::make_unique<Node>(context, config(dir.path() / "a", key));
  node->start();
  node->stop();
  work.reset();
  context.stop();
  for (auto& thread : threads) thread.join();
  // On a thread of its own, so a hang fails this test rather than the suite.
  auto destroyed = std::make_shared<std::promise<void>>();
  std::thread([destroyed, held = node.release()] {
    delete held;
    destroyed->set_value();
  }).detach();
  EXPECT_EQ(destroyed->get_future().wait_for(10s), std::future_status::ready);
}

// The transport writes through the sink the node was given, so a hosted
// system's own sink keeps its line format.
TEST(Node, TheTransportWritesThroughTheNodesSink) {
  auto log = std::make_shared<test::RecordingSink>();
  Pair pair([&](NodeConfig& b) { b.log = log; });
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::info, "node connection outbound peer=") >= 1; }));
}

TEST(Node, ADialHookInTheConfigRefusesADial) {
  std::atomic<int> refused{0};
  Pair pair([&](NodeConfig& b) {
    b.dial_hook = [&](const net::Endpoint&, net::Lane) {
      ++refused;
      return std::string("link is down");
    };
  });
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return refused.load() >= 1; }));
  EXPECT_FALSE(knows(*pair.b, pair.a->context().identity().id()));
}

// The memory limit, as Macha's configuration has it: every part shrinks by the
// same fraction, the hosted system's included, and one warning is logged.
TEST(Node, TheMemoryLimitShrinksEveryPartAndWarnsOnce) {
  test::TempDir dir;
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  auto settings = config(dir.path() / "a", cluster_key(dir.path()));
  settings.log = log;
  settings.memory_limit_bytes = 400ull << 20;
  settings.memory_bytes = 600ull << 20;
  settings.control_reserve_bytes = 60ull << 20;
  settings.foreground_reserve_bytes = 60ull << 20;
  settings.loader_reserve_bytes = 60ull << 20;
  settings.reassembly_reserve_bytes = 60ull << 20;
  std::vector<sched::MemoryPart> hosted{{"cache", 200ull << 20}};
  fit_memory_limit(settings, hosted);
  EXPECT_EQ(hosted[0].bytes, 100ull << 20);
  EXPECT_EQ(settings.memory_bytes, 300ull << 20);
  EXPECT_EQ(settings.control_reserve_bytes, 30ull << 20);
  Node node(io.context, settings);
  EXPECT_EQ(log->count(status::LogLevel::warn, "MEMORY OVER LIMIT: the configured parts total 800M, over runtime.memory_bytes 400M"), 1u);
  EXPECT_EQ(log->count(status::LogLevel::warn, "cache 200M to 100M"), 1u);
}

TEST(Node, ANodeWithNoHostedPartsFitsItselfAndUnderTheLimitNothingShrinks) {
  test::TempDir dir;
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  auto settings = config(dir.path() / "a", cluster_key(dir.path()));
  settings.log = log;
  settings.memory_limit_bytes = 1ull << 40;
  {
    Node roomy(io.context, settings);
  }
  EXPECT_EQ(log->count(status::LogLevel::warn, "MEMORY OVER LIMIT"), 0u);
  settings.memory_limit_bytes = 100ull << 20;
  settings.state_path = dir.path() / "b";
  Node tight(io.context, settings);
  EXPECT_EQ(log->count(status::LogLevel::warn, "MEMORY OVER LIMIT"), 1u);
}

// The node's one wait guard, reached through the Context, logs through its sink.
TEST(Node, TheWaitGuardIsTheNodesOwn) {
  auto log = std::make_shared<test::RecordingSink>();
  test::TempDir dir;
  Io io;
  auto settings = config(dir.path() / "a", cluster_key(dir.path()));
  settings.log = log;
  Node node(io.context, settings);
  const sched::WorkContext context(sched::FrameType::control, {}, nullptr, "a test", sched::Waits::none);
  EXPECT_FALSE(node.context().wait_guard().enter(context, sched::Waits::network, "a blocking thing"));
  EXPECT_EQ(node.context().wait_guard().violations(), 1u);
  EXPECT_EQ(log->count(status::LogLevel::warn, "a blocking thing"), 1u);
}

TEST(Node, ObservationsAreWrittenFromTheStart) {
  test::TempDir dir;
  Io io;
  auto settings = config(dir.path() / "a", cluster_key(dir.path()));
  settings.observation_interval = 10ms;
  settings.observation_gauges = [] { return std::map<std::string, uint64_t>{{"parked", 7}}; };
  Node node(io.context, settings);
  node.start();
  const auto path = dir.path() / "a" / "observation" / "observations.jsonl";
  const auto contents = [&] {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  EXPECT_TRUE(eventually([&] {
    const auto text = contents();
    return text.find("0.1.0-test") != std::string::npos && text.find("parked") != std::string::npos;
  })) << contents();
  node.stop();
}

TEST(Node, ObservationsCanBeTurnedOff) {
  test::TempDir dir;
  Io io;
  auto settings = config(dir.path() / "a", cluster_key(dir.path()));
  settings.observation_interval = 0ms;
  Node node(io.context, settings);
  node.start();
  node.stop();
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "a" / "observation" / "observations.jsonl"));
}

// A forced refresh runs now, on the node's loop rather than the caller's thread,
// and answers on the caller's executor; a node not running answers empty.
TEST(Node, APublicConnectivityRefreshRunsWhenAskedAndAnswersOnTheExecutor) {
  test::TempDir dir;
  Io io;
  std::atomic<int> probes{0};
  auto settings = config(dir.path() / "a", cluster_key(dir.path()));
  settings.heartbeat = 1h;
  settings.connectivity.check.enabled = false;
  settings.self_probe = [&](const net::Endpoint&, std::chrono::milliseconds, std::string&) {
    ++probes;
    return true;
  };
  Node node(io.context, settings);
  const auto ask = [&] {
    std::promise<std::optional<reach::PublicConnectivityStatus>> answer;
    auto future = answer.get_future();
    node.context().nodes().refresh_public_connectivity(true, true, io.context.get_executor(), [&](auto status) { answer.set_value(std::move(status)); });
    EXPECT_EQ(future.wait_for(10s), std::future_status::ready);
    return future.get();
  };
  EXPECT_FALSE(ask().has_value());
  node.start();
  const auto asked = probes.load();
  EXPECT_TRUE(ask().has_value());
  EXPECT_GT(probes.load(), asked);
  node.stop();
  EXPECT_FALSE(ask().has_value());
}

// Gossip waits out its cap however much demand there is: wakes sample the local
// record, and only the injected second lets one out.
TEST(Node, GossipWaitsOutItsCapHoweverMuchDemandThereIs) {
  test::SteppedTime time;
  Pair pair([&](NodeConfig& b) {
    b.time = &time;
    b.telemetry_interval = 1h;
  });
  pair.b->start();
  const auto b = pair.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return pair.b->context().nodes().directly_reachable(pair.a->context().identity().id()); }));
  const auto seen_at_a = [&]() -> uint64_t {
    for (const auto& record : pair.a->context().telemetry().all())
      if (record.node_id() == b) return record.sequence();
    return 0;
  };
  const auto local = [&] { return pair.b->context().telemetry().local()->sequence(); };
  ASSERT_TRUE(eventually([&] {
    time.advance(1s);
    pair.b->context().telemetry().refresh();
    return seen_at_a() > 0;
  }));
  // Time stands still from here. A tick or two lets out what the last step
  // allowed; after that the record moves on and the gossip does not.
  const auto settling = local();
  ASSERT_TRUE(eventually([&] {
    pair.b->context().telemetry().refresh();
    return local() > settling + 2;
  }));
  std::this_thread::sleep_for(50ms);
  const auto before = local();
  ASSERT_TRUE(eventually([&] {
    pair.b->context().telemetry().refresh();
    return local() > before + 5;
  }));
  std::this_thread::sleep_for(50ms);
  EXPECT_LE(seen_at_a(), before);
  // Read before the step: what it lets out may be sampled before a later wake
  // moves the record on, and then nothing more goes until time moves again.
  // Each try steps again: the round a step allows is best effort, and sends
  // nothing while the route table is busy, so one round may go nowhere.
  const auto after = local();
  EXPECT_TRUE(eventually([&] {
    time.advance(1s);
    pair.b->context().telemetry().refresh();
    return seen_at_a() >= after;
  }));
}

// A peer is told of this node on observing it, not at the next interval.
TEST(Node, APeerObservationGossipsSoonerThanTheInterval) {
  Pair pair([](NodeConfig& b) { b.telemetry_interval = 1h; });
  pair.b->start();
  const auto b = pair.b->context().identity().id();
  EXPECT_TRUE(eventually([&] {
    const auto all = pair.a->context().telemetry().all();
    return std::any_of(all.begin(), all.end(), [&](const status::Telemetry& record) { return record.node_id() == b; });
  }));
}

// A node watches no call unless asked; a call asks for itself.
TEST(Node, ACallIsWatchedOnlyIfItAsks) {
  EXPECT_EQ(NodeConfig{}.control_no_progress, 0ms);
  EXPECT_EQ(NodeConfig{}.data_no_progress, 0ms);
  EXPECT_EQ(NodeConfig{}.control_stall_notice, 0ms);
  EXPECT_EQ(NodeConfig{}.data_stall_notice, 0ms);
  constexpr net::MessageType held{1301};
  test::Gate gate;
  Pair pair({}, [&](Node& a) {
    a.context().routes().add(held, {"held", net::Lanes::control, net::Frames::control_only, false},
                             [&](const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
                               gate.enter_and_wait();
                               return net::Handled::processed({net::message::ok, request.payload, {}});
                             });
  });
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.b, a); }));
  const net::Endpoint at{"127.0.0.1", pair.a->port()};
  const auto watched = pair.b->context().routes().call_and_wait(at, held, {}, sched::FrameType::control, 10s, net::CallWatch{100ms, std::nullopt});
  EXPECT_FALSE(watched.reply);
  EXPECT_NE(watched.failure.find("cancelled for retry"), std::string::npos) << watched.failure;
  std::thread opener([&] {
    std::this_thread::sleep_for(300ms);
    gate.open();
  });
  const auto unwatched = pair.b->context().routes().call_and_wait(at, held, {}, sched::FrameType::control, 10s);
  opener.join();
  EXPECT_TRUE(unwatched.reply) << unwatched.failure;
}

// The owners' test seam (P6): which of B's calls its fault takes, set once A is
// up: the calls to one peer of the types named. The node's own calls (its
// membership exchange) are never named, so they go on.
struct FaultPlan {
  std::mutex mutex;
  std::optional<identity::NodeId> peer;
  std::set<net::MessageType> types;
  net::CallFault fault;
  std::atomic<int> taken{0};

  void set(identity::NodeId to, std::set<net::MessageType> of, net::CallFault what) {
    std::lock_guard lock(mutex);
    peer = to;
    types = std::move(of);
    fault = std::move(what);
  }
  static std::function<net::CallFault(const std::optional<identity::NodeId>&, const net::Endpoint&, net::MessageType)> hook(std::shared_ptr<FaultPlan> plan) {
    return [plan](const std::optional<identity::NodeId>& to, const net::Endpoint&, net::MessageType of) {
      std::lock_guard lock(plan->mutex);
      if (plan->fault.kind == net::CallFault::Kind::none || to != plan->peer || !plan->types.contains(of)) return net::CallFault{};
      ++plan->taken;
      return plan->fault;
    };
  }
};

// The node's record of `id`, once it lists it as active.
std::optional<membership::NodeRecord> record_of(const Node& node, const identity::NodeId& id) {
  for (const auto& each : node.context().nodes().active())
    if (each.id == id) return each;
  return std::nullopt;
}

// Two types on A that count their calls, for the fault's tests.
constexpr net::MessageType faulted_type{1311};
constexpr net::MessageType passed_type{1312};
std::function<void(Node&)> counted_types(std::atomic<int>& handled) {
  return [&handled](Node& a) {
    for (const auto type : {faulted_type, passed_type})
      a.context().routes().add(type, {type == faulted_type ? "faulted" : "passed", net::Lanes::control, net::Frames::control_only, false},
                               [&handled](const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
                                 ++handled;
                                 return net::Handled::processed({net::message::ok, request.payload, {}});
                               });
  };
}

// A refused call fails at once with the fault's reason and never reaches the
// peer, by record and by an address the node has met; another type passes.
TEST(Node, ACallFaultRefusesACallByPeerAndTypeBeforeTheWire) {
  std::atomic<int> handled{0};
  auto plan = std::make_shared<FaultPlan>();
  Pair pair([&](NodeConfig& b) { b.call_fault = FaultPlan::hook(plan); }, counted_types(handled));
  const auto a = pair.a->context().identity().id();
  plan->set(a, {faulted_type}, net::CallFault::refuse("link refused by the test"));
  pair.b->start();
  ASSERT_TRUE(eventually([&] { return record_of(*pair.b, a).has_value(); }));
  const auto record = *record_of(*pair.b, a);
  // The passed type first: its call connects B's client to A, so A's address is one B has met.
  // Made later, the address call raced the node's own first connection and could go out unknown
  // (TSan in 0.10.1's gate on the build cluster).
  const auto passed = pair.b->context().routes().call_and_wait(record, passed_type, {}, FrameType::control, 5s);
  EXPECT_TRUE(passed.reply) << passed.failure;
  const auto refused = pair.b->context().routes().call_and_wait(record, faulted_type, {}, FrameType::control, 5s);
  EXPECT_FALSE(refused.reply);
  EXPECT_EQ(refused.failure, "link refused by the test");
  const auto by_address = pair.b->context().routes().call_and_wait(net::Endpoint{"127.0.0.1", pair.a->port()}, faulted_type, {}, FrameType::control, 5s);
  EXPECT_EQ(by_address.failure, "link refused by the test");
  EXPECT_EQ(handled.load(), 1);
  EXPECT_EQ(plan->taken.load(), 2);
}

// A stalled call is held unanswered, as one to a dead link: its idleness grows
// from when it was made, nothing reaches the peer, and its deadline ends it.
TEST(Node, AStalledCallIsHeldUnansweredUntilItsDeadline) {
  std::atomic<int> handled{0};
  auto plan = std::make_shared<FaultPlan>();
  Pair pair([&](NodeConfig& b) { b.call_fault = FaultPlan::hook(plan); }, counted_types(handled));
  const auto a = pair.a->context().identity().id();
  plan->set(a, {faulted_type}, net::CallFault::stall());
  pair.b->start();
  ASSERT_TRUE(eventually([&] { return record_of(*pair.b, a).has_value(); }));
  auto outcome = std::make_shared<std::promise<net::CallOutcome>>();
  auto ended = outcome->get_future();
  const auto handle = pair.b->context().routes().call(*record_of(*pair.b, a), faulted_type, {}, FrameType::control, 600ms, pair.io.context.get_executor(),
                                                      [outcome](net::CallOutcome out) { outcome->set_value(std::move(out)); });
  EXPECT_EQ(pair.b->stalled_calls(), 1u);
  EXPECT_TRUE(eventually([&] { return handle->idle_for() >= 200ms; }));
  ASSERT_EQ(ended.wait_for(10s), std::future_status::ready);
  const auto out = ended.get();
  EXPECT_FALSE(out.reply);
  EXPECT_NE(out.failure.find("deadline of 600 ms"), std::string::npos) << out.failure;
  EXPECT_EQ(pair.b->stalled_calls(), 0u);
  EXPECT_EQ(handled.load(), 0);
}

// A stalled call is watched as a placed one is: its no-progress watch cancels
// it for retry, long before its deadline.
TEST(Node, AStalledCallIsCancelledByItsWatch) {
  std::atomic<int> handled{0};
  auto plan = std::make_shared<FaultPlan>();
  Pair pair([&](NodeConfig& b) { b.call_fault = FaultPlan::hook(plan); }, counted_types(handled));
  const auto a = pair.a->context().identity().id();
  plan->set(a, {faulted_type}, net::CallFault::stall());
  pair.b->start();
  ASSERT_TRUE(eventually([&] { return record_of(*pair.b, a).has_value(); }));
  const auto began = std::chrono::steady_clock::now();
  const auto watched =
      pair.b->context().routes().call_and_wait(*record_of(*pair.b, a), faulted_type, {}, FrameType::control, 20s, net::CallWatch{150ms, std::nullopt});
  EXPECT_LT(std::chrono::steady_clock::now() - began, 10s);
  EXPECT_FALSE(watched.reply);
  EXPECT_NE(watched.failure.find("cancelled for retry"), std::string::npos) << watched.failure;
  EXPECT_EQ(pair.b->stalled_calls(), 0u);
  EXPECT_EQ(handled.load(), 0);
}

// Releasing a stall fails the calls it holds, as a link that timed out, and
// stopping the node's calls fails those held after.
TEST(Node, ReleasingAStallFailsItsHeldCallsAndCancellingFailsTheRest) {
  std::atomic<int> handled{0};
  auto plan = std::make_shared<FaultPlan>();
  Pair pair([&](NodeConfig& b) { b.call_fault = FaultPlan::hook(plan); }, counted_types(handled));
  const auto a = pair.a->context().identity().id();
  pair.b->start();
  ASSERT_TRUE(eventually([&] { return record_of(*pair.b, a).has_value(); }));
  const auto record = *record_of(*pair.b, a);
  // Every call to A of both types from here.
  plan->set(a, {faulted_type, passed_type}, net::CallFault::stall());
  std::vector<std::shared_ptr<std::promise<net::CallOutcome>>> outcomes;
  std::vector<std::future<net::CallOutcome>> ends;
  const auto place = [&](net::MessageType type) {
    auto outcome = std::make_shared<std::promise<net::CallOutcome>>();
    ends.push_back(outcome->get_future());
    outcomes.push_back(outcome);
    pair.b->context().routes().call(record, type, {}, FrameType::control, 30s, pair.io.context.get_executor(),
                                    [outcome](net::CallOutcome out) { outcome->set_value(std::move(out)); });
  };
  place(faulted_type);
  place(passed_type);
  EXPECT_EQ(pair.b->stalled_calls(), 2u);
  EXPECT_EQ(pair.b->release_stalled_calls(identity::NodeId::random()), 0u);
  EXPECT_EQ(pair.b->release_stalled_calls(a), 2u);
  for (size_t i = 0; i < 2; ++i) {
    ASSERT_EQ(ends[i].wait_for(10s), std::future_status::ready);
    EXPECT_EQ(ends[i].get().failure, "stall released");
  }
  place(faulted_type);
  EXPECT_EQ(pair.b->stalled_calls(), 1u);
  pair.b->cancel_outbound_calls();
  ASSERT_EQ(ends[2].wait_for(10s), std::future_status::ready);
  EXPECT_EQ(ends[2].get().failure, "client stopped");
  EXPECT_EQ(pair.b->stalled_calls(), 0u);
  EXPECT_EQ(handled.load(), 0);
}

// A hosted system's handler may answer later (P8): bound with
// bind_deferred, it keeps the request's Answer and gives it from another
// thread; the caller on the other node gets that reply, and a request it
// drops is refused rather than left to the caller's deadline.
TEST(Node, AHostedHandlerAnswersLaterFromAnotherThread) {
  constexpr net::MessageType later{1302};
  std::mutex mutex;
  std::vector<net::Answer> kept;
  Pair pair({}, [&](Node& a) {
    a.context().routes().add(later, {"later", net::Lanes::data, net::Frames::not_control, false});
    a.context().routes().bind_deferred(later, [&](const membership::NodeRecord&, sched::FrameType, const net::Message& request, net::Answer& answer) {
      if (request.payload == net::Bytes{0}) return net::Handled::deferred();  // dropped: answer stays in the call
      std::lock_guard lock(mutex);
      kept.push_back(std::move(answer));
      return net::Handled::deferred();
    });
  });
  // Both ends know the type: its lane is the caller's to choose.
  pair.b->context().routes().add(later, {"later", net::Lanes::data, net::Frames::not_control, false});
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.b, a); }));
  const net::Endpoint at{"127.0.0.1", pair.a->port()};
  std::thread committer([&] {
    for (int spin = 0; spin < 2000; ++spin) {
      {
        std::lock_guard lock(mutex);
        if (!kept.empty()) {
          kept.front()(net::Message{net::message::ok, net::Bytes{7}, {}});
          kept.clear();
          return;
        }
      }
      std::this_thread::sleep_for(5ms);
    }
  });
  const auto answered = pair.b->context().routes().call_and_wait(at, later, net::Bytes{1}, sched::FrameType::foreground, 10s);
  committer.join();
  {
    std::lock_guard lock(mutex);
    EXPECT_TRUE(kept.empty()) << kept.size() << " kept but never answered";
  }
  ASSERT_TRUE(answered.reply) << answered.failure;
  EXPECT_EQ(answered.reply->payload, net::Bytes{7});
  const auto dropped = pair.b->context().routes().call_and_wait(at, later, net::Bytes{0}, sched::FrameType::foreground, 10s);
  ASSERT_TRUE(dropped.reply) << dropped.failure;
  const auto refusal = net::refusal_of(*dropped.reply);
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->code, "abandoned");
}

// The node's inbound answer is read through a const node, as a host's Status
// page holds it.
TEST(Node, TheInboundAnswerReadsThroughAConstNode) {
  Pair pair;
  const Node& a = *pair.a;
  EXPECT_EQ(a.inbound().capable(), pair.a->inbound().capable());
  EXPECT_EQ(a.inbound().state().source, pair.a->inbound().state().source);
}

// Asking the loops to stop returns at once; stop() afterwards still finishes it.
TEST(Node, RequestStopAsksAndStopFinishes) {
  Pair pair;
  pair.b->start();
  pair.b->request_stop();
  pair.b->request_stop();
  pair.b->stop();
  EXPECT_TRUE(pair.b->context().readiness().control_plane_online() == false);
}

TEST(Node, ThreadsAreRecordedInDiagnostics) {
  Pair pair;
  std::set<std::string> names;
  EXPECT_TRUE(eventually([&] {
    names.clear();
    for (const auto& thread : pair.a->context().threads().statuses()) names.insert(thread.name);
    return names.contains("net-rpc-control-worker") && names.contains("net-rpc-metadata-worker") && names.contains("reach-path-worker");
  }));
}

// Each node names the other as its seed: they settle on one session a lane and stop
// dialling.
TEST(Node, MutualSeedsSettleOnOneSessionAndStopDialling) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  const uint16_t first_port = test::free_port();
  const uint16_t second_port = test::free_port();
  auto a_config = config(dir.path() / "a", key, {{"127.0.0.1", second_port}});
  a_config.port = first_port;
  auto b_config = config(dir.path() / "b", key, {{"127.0.0.1", first_port}});
  b_config.port = second_port;
  Node a(io.context, a_config), b(io.context, b_config);
  a.start();
  b.start();
  const auto a_id = a.context().identity().id();
  const auto b_id = b.context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(a, b_id) && knows(b, a_id); }));
  ASSERT_TRUE(eventually([&] { return a.diagnostics().work.sessions == 1 || b.diagnostics().work.sessions == 1; }));
  // Quiet: several heartbeats pass, and neither dials again.
  std::this_thread::sleep_for(300ms);
  const auto a_dials = a.diagnostics().client.dials;
  const auto b_dials = b.diagnostics().client.dials;
  std::this_thread::sleep_for(1s);
  EXPECT_EQ(a.diagnostics().client.dials, a_dials);
  EXPECT_EQ(b.diagnostics().client.dials, b_dials);
  EXPECT_LE(a.diagnostics().work.sessions + b.diagnostics().work.sessions, 2u);
  b.stop();
  a.stop();
}

// What one node says it sent is what the other says it received, class by class.
TEST(Node, TrafficAgreesAcrossTwoNodesByClass) {
  constexpr net::MessageType echo{1302};
  Pair pair({}, [&](Node& a) {
    a.context().routes().add(
        echo, {"echo", net::Lanes::both, net::Frames::any, false},
        [](const membership::NodeRecord&, sched::FrameType, const net::Message&) { return net::Handled::processed({net::message::ok, {}, {}}); });
  });
  pair.b->start();
  ASSERT_TRUE(eventually([&] { return knows(*pair.b, pair.a->context().identity().id()); }));
  const net::Endpoint at{"127.0.0.1", pair.a->port()};
  const auto outcome = pair.b->context().routes().call_and_wait(at, echo, net::Bytes(10000, 1), sched::FrameType::foreground, 10s);
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  const auto index = static_cast<size_t>(sched::FrameType::foreground);
  // One request of 10,000 bytes and its overhead, and an empty reply.
  EXPECT_TRUE(eventually([&] {
    const auto sent = pair.b->diagnostics().traffic;
    const auto got = pair.a->diagnostics().traffic;
    return sent.out_bytes[index] == got.in_bytes[index] && sent.in_bytes[index] == got.out_bytes[index];
  }));
  EXPECT_EQ(pair.b->diagnostics().traffic.out_bytes[index], 10056u);
}

// A number given up is not reused, and not re-registered by mistake.
TEST(Node, ARetiredMessageNumberIsRefused) {
  test::TempDir dir;
  Io io;
  Node node(io.context, config(dir.path() / "a", cluster_key(dir.path())));
  node.context().routes().retire(net::MessageType{549});
  EXPECT_THROW(node.context().routes().add(net::MessageType{549}, {"taken", net::Lanes::control, net::Frames::control_only, false}), std::invalid_argument);
  EXPECT_THROW(node.context().routes().retire(net::MessageType{1}), std::invalid_argument);
  node.start();
  EXPECT_THROW(node.context().routes().retire(net::MessageType{550}), std::logic_error);
  node.stop();
}

// Over real dial-backs: a node nobody can reach goes incapable once confirmed, keeps
// that across a restart, and comes back once its address works.
TEST(Node, AnUnreachableAutomaticNodeGoesIncapableAndPeersStopDialling) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  auto hub_config = config(dir.path() / "hub", key);
  hub_config.dial_back_min_interval = 10ms;
  Node hub(io.context, hub_config);
  hub.start();
  const uint16_t site_port = test::free_port();
  const auto site_settings = [&](const std::string& advertised) {
    auto out = config(dir.path() / "site", key, {{"127.0.0.1", hub.port()}});
    out.inbound = reach::Inbound::automatic;
    out.port = site_port;
    out.advertise_host = advertised;
    out.reprobe_while_capable = 100ms;
    out.reprobe_while_incapable = 100ms;
    return out;
  };
  identity::NodeId site_id;
  {
    Node site(io.context, site_settings("192.0.2.1"));
    site_id = site.context().identity().id();
    site.start();
    EXPECT_TRUE(eventually([&] { return !site.context().nodes().inbound_capable(site_id); }, 30s));
    EXPECT_TRUE(eventually([&] { return !hub.context().nodes().inbound_capable(site_id); }, 30s));
    site.stop();
  }
  {
    Node site(io.context, site_settings("127.0.0.1"));
    EXPECT_FALSE(site.context().nodes().inbound_capable(site_id));
    site.start();
    EXPECT_TRUE(eventually([&] { return site.context().nodes().inbound_capable(site_id); }, 30s));
    EXPECT_TRUE(eventually([&] { return hub.context().nodes().inbound_capable(site_id); }, 30s));
    site.stop();
  }
  hub.stop();
}

TEST(Node, WhatTheNodeDoesIsJournalled) {
  auto log = std::make_shared<test::RecordingSink>();
  Pair pair([&](NodeConfig& b) {
    b.log = log;
    b.bootstrap.push_back({"127.0.0.1", test::free_port()});
  });
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "bootstrap: ") >= 1; }));
  membership::IdentityReset reset;
  reset.host = "192.0.2.9";
  reset.port = 7;
  reset.epoch = 3;
  reset.reset_by = pair.b->context().identity().id();
  reset.reason = "testing";
  EXPECT_TRUE(pair.b->context().nodes().apply_identity_reset(reset));
  EXPECT_EQ(log->count(status::LogLevel::info, "node identity association reset scope=[192.0.2.9]:7 stale_node_id=<any> epoch=3"), 1u);
  EXPECT_EQ(log->count(status::LogLevel::info, "reason=testing"), 1u);
}

TEST(Node, ACorruptPersistedFileIsSaidAndSurvived) {
  test::TempDir dir;
  Io io;
  auto log = std::make_shared<test::RecordingSink>();
  std::filesystem::create_directories(dir.path() / "a" / "connectivity");
  std::filesystem::create_directories(dir.path() / "a" / "telemetry");
  std::ofstream(dir.path() / "a" / "connectivity" / "inbound.bin") << "not this";
  std::ofstream(dir.path() / "a" / "telemetry" / "last-known.bin") << "not this";
  auto settings = config(dir.path() / "a", cluster_key(dir.path()));
  settings.log = log;
  settings.inbound = reach::Inbound::automatic;
  Node node(io.context, settings);
  EXPECT_EQ(log->count(status::LogLevel::warn, "inbound resolution ignored: "), 1u);
  EXPECT_EQ(log->count(status::LogLevel::warn, "persisted telemetry ignored: "), 1u);
}

// Status reads Macha's names for the three loops; ruling B, 2026-11-05.
// P4: the node's worker pool runs with the node, and a hosted system's own pool
// runs beside it on the node's gates, its threads under the node's supervision.
TEST(Node, TheNodesPoolRunsWithItAndAHostedPoolBesideIt) {
  Pair pair;
  std::atomic<int> ran{0};
  ASSERT_TRUE(pair.a->context().workers().submit(sched::WorkContext(FrameType::foreground), [&](sched::WorkerPool::Turn&) { ++ran; }).accepted);
  sched::WorkerPoolConfig own;
  own.threads = 1;
  own.name = "hash";
  own.reserve = {0, 0, 0, 0};
  sched::WorkerPool hashing(own, pair.a->context().gates(), &pair.a->context().threads());
  hashing.start();
  ASSERT_TRUE(hashing.submit(sched::WorkContext(FrameType::foreground), [&](sched::WorkerPool::Turn&) { ++ran; }).accepted);
  EXPECT_TRUE(eventually([&] { return ran.load() == 2; }));
  std::set<std::string> names;
  for (const auto& thread : pair.a->context().threads().statuses()) names.insert(thread.name);
  EXPECT_TRUE(names.contains("work-0"));
  EXPECT_TRUE(names.contains("hash-0"));
  EXPECT_EQ(&pair.a->context().gates().gate(sched::WorkClass::speculative), &pair.a->gates().gate(sched::WorkClass::speculative));
  hashing.stop();
  pair.a->stop();
  EXPECT_EQ(pair.a->context().workers().submit(sched::WorkContext(FrameType::foreground), [](sched::WorkerPool::Turn&) {}).why,
            sched::WorkerPool::Refused::stopped);
}

// The gates keep their own time, apart from the node's: a hosted system's pacing
// tests step it while the node runs on real time (Macha's review).
TEST(Node, TheGatesRunOnTheirOwnTime) {
  test::SteppedTime paced;
  Pair pair([&](NodeConfig& b) { b.gate_time = &paced; });
  pair.b->start();
  auto& gate = pair.b->context().gates().gate(sched::WorkClass::speculative);
  gate.set_contended([] { return true; });
  gate.started();
  paced.advance(10ms);
  // 95:5 by default: 10 ms earns 190 ms, in the gates' time alone.
  EXPECT_EQ(gate.finished(), 190ms);
  EXPECT_EQ(gate.rest(), 190ms);
  paced.advance(190ms);
  EXPECT_EQ(gate.rest(), 0ms);
  gate.set_contended({});
}

TEST(Node, TheLoopsCarryTheirNames) {
  Pair pair;
  std::set<std::string> names;
  EXPECT_TRUE(eventually([&] {
    names.clear();
    for (const auto& thread : pair.a->context().threads().statuses()) names.insert(thread.name);
    return names.contains("cluster-telemetry") && names.contains("cluster-maintenance") && names.contains("cluster-connectivity");
  }));
  EXPECT_FALSE(names.contains("node-telemetry"));
}

// The record reports the transport's counts, not a guess from the peers it knows.
TEST(Node, TheTelemetryRecordCarriesTheTransportsCounts) {
  Pair pair;
  pair.b->start();
  const auto b = pair.b->context().identity().id();
  EXPECT_TRUE(eventually([&] {
    const auto local = pair.b->context().telemetry().local();
    return local && local->u64(status::field::connections_created) >= 1 && local->u64(status::field::connections_canonical) >= 1;
  }));
  (void)b;
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

// A call whose deadline passes is cancelled, not left running: it holds no
// reply slot after it (Dagda asked, 2026-11-04).
TEST(Node, ACallFreesItsReplySlotAtItsDeadline) {
  constexpr net::MessageType slow{1313};
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
  // Placed first, so the deadline below finds the call pending on a session.
  std::promise<net::CallOutcome> done;
  auto handle = pair.b->context().routes().call(pair.a->context().nodes().self(), slow, {}, FrameType::control, 1500ms, pair.io.context.get_executor(),
                                                [&](net::CallOutcome outcome) { done.set_value(std::move(outcome)); });
  EXPECT_TRUE(eventually([&] { return handle->idle_for() != std::chrono::milliseconds::max(); }));
  const auto outcome = done.get_future().get();
  EXPECT_NE(outcome.failure.find("deadline"), std::string::npos) << outcome.failure;
  EXPECT_TRUE(eventually([&] { return handle->idle_for() == std::chrono::milliseconds::max(); }));
  release.set_value();
}

// The node's time source and executor reach the hosted system.
TEST(Node, TheContextGivesTheTimeSourceAndExecutor) {
  struct Fixed final : contract::TimeSource {
    contract::TimePoint now() const noexcept override { return contract::TimePoint(1h); }
  } fixed;
  Pair pair([&](NodeConfig& c) { c.time = &fixed; });
  auto& time = pair.b->context().time();
  EXPECT_EQ(&time.source(), &fixed);
  EXPECT_EQ(time.source().now(), contract::TimePoint(1h));
  std::promise<void> ran;
  boost::asio::post(time.executor(), [&] { ran.set_value(); });
  EXPECT_EQ(ran.get_future().wait_for(5s), std::future_status::ready);
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

// A hosted system declares its own planes through the Context before the
// node starts, beside those NodeConfig lists; once started, the planes are
// fixed (Dagda asked, 2026-11-04).
TEST(Node, AHostedSystemDeclaresItsPlanesBeforeStart) {
  Pair pair([](NodeConfig& c) { c.planes = {"metadata"}; });
  auto& readiness = pair.b->context().readiness();
  readiness.declare("dagda.store");
  readiness.declare("metadata");
  EXPECT_THROW(readiness.declare(""), std::invalid_argument);
  pair.b->start();
  EXPECT_THROW(readiness.declare("late"), std::logic_error);
  readiness.ready("metadata");
  EXPECT_EQ(readiness.phase(), status::Phase::recovering);
  readiness.ready("dagda.store");
  EXPECT_EQ(readiness.phase(), status::Phase::ready);
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

// A hosted system's charges show under its own owner; Dannan's owners are not
// its to name (Dagda's ask: its reads show as object payload).
TEST(Node, ABudgetChargesTheHostedOwnerNamed) {
  Pair pair;
  auto& budget = pair.a->context().budget();
  const auto payload = static_cast<sched::MemoryOwner>(static_cast<uint16_t>(sched::MemoryOwner::hosted_first) + 3);
  auto lease = budget.reserve(sched::WorkClass::foreground, payload, 4096);
  ASSERT_TRUE(lease);
  EXPECT_EQ(budget.stats().owner_bytes.at(payload), 4096u);
  lease.reset();
  const auto after = budget.stats().owner_bytes;
  EXPECT_TRUE(!after.contains(payload) || after.at(payload) == 0u);
  EXPECT_THROW((void)budget.reserve(sched::WorkClass::foreground, sched::MemoryOwner::rpc_frame, 1), std::invalid_argument);
  EXPECT_THROW((void)budget.acquire(sched::WorkClass::foreground, sched::MemoryOwner::rpc_frame, 1, contract::Clock::now() + 1s), std::invalid_argument);
  // No call waits without a deadline.
  EXPECT_THROW((void)budget.acquire(sched::WorkClass::foreground, payload, 1, contract::TimePoint{}), std::invalid_argument);
}

// A read waits for room under its deadline and its cancel flag, as Macha's do
// on the ledger (distributed_store.cpp:1744): room let go admits the waiter,
// a deadline that passes or a cancel ends the wait empty.
TEST(Node, ABudgetWaitsForRoomUnderADeadlineAndACancel) {
  Pair pair([](NodeConfig& c) {
    c.memory_bytes = 64ull << 20;
    c.control_reserve_bytes = 8ull << 20;
    c.foreground_reserve_bytes = 8ull << 20;
    c.loader_reserve_bytes = 8ull << 20;
    c.reassembly_reserve_bytes = 8ull << 20;
  });
  auto& budget = pair.a->context().budget();
  const auto owner = sched::MemoryOwner::hosted_first;
  // Take all the loader class may hold, a MiB at a time.
  std::vector<sched::RetainedMemoryLedger::Lease> held;
  while (auto lease = budget.reserve(sched::WorkClass::loader, owner, 1ull << 20)) held.push_back(std::move(*lease));
  ASSERT_FALSE(held.empty());

  EXPECT_FALSE(budget.acquire(sched::WorkClass::loader, owner, 1ull << 20, contract::Clock::now() + 50ms));

  std::atomic_bool cancelled{false};
  auto cancelled_wait = std::async(
      std::launch::async, [&] { return budget.acquire(sched::WorkClass::loader, owner, 1ull << 20, contract::Clock::now() + 60s, &cancelled).has_value(); });
  EXPECT_EQ(cancelled_wait.wait_for(50ms), std::future_status::timeout);
  cancelled = true;
  budget.wake();
  ASSERT_EQ(cancelled_wait.wait_for(10s), std::future_status::ready);
  EXPECT_FALSE(cancelled_wait.get());

  auto admitted = std::async(std::launch::async, [&] { return budget.acquire(sched::WorkClass::loader, owner, 1ull << 20, contract::Clock::now() + 60s); });
  EXPECT_EQ(admitted.wait_for(50ms), std::future_status::timeout);
  held.pop_back();
  ASSERT_EQ(admitted.wait_for(10s), std::future_status::ready);
  auto lease = admitted.get();
  ASSERT_TRUE(lease);
  EXPECT_EQ(lease->bytes(), 1ull << 20);
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

// A node with key custody opens it at start: its public key, signatures
// by it, and node-local keys reach the hosted system through the Context;
// without custody those refuse. The cluster-wide derived key is in secure
// memory and the same on every node.
TEST(Node, OpensItsKeyCustody) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  const auto source = identity::KeySource::typed_passphrase(
      [] {
        const std::string p = "node passphrase";
        return identity::Secret(std::span(reinterpret_cast<const uint8_t*>(p.data()), p.size()));
      },
      identity::Argon2Costs{1024, 1, 1});
  (void)identity::KeyCustody::create(dir.path() / "a", source);
  auto with = config(dir.path() / "a", key);
  with.custody = source;
  Node a(io.context, with);
  Node b(io.context, config(dir.path() / "b", key));
  const auto& identity = a.context().identity();
  ASSERT_TRUE(identity.has_custody());
  const net::Bytes message{1, 2, 3};
  EXPECT_TRUE(identity::verify(identity::SigningAlgorithm::es256, identity.node_public_key(), message, identity.sign(message)));
  EXPECT_EQ(identity.derive_local("dagda/node-local/1").size(), 32u);
  EXPECT_FALSE(b.context().identity().has_custody());
  EXPECT_TRUE(b.context().identity().node_public_key().empty());
  EXPECT_THROW(b.context().identity().sign(message), std::logic_error);
  EXPECT_THROW(b.context().identity().derive_local("x"), std::logic_error);
  EXPECT_TRUE(a.context().identity().derive_key("lir/1") == b.context().identity().derive_key("lir/1"));
  // Custody named but never made: the node refuses to start, saying so.
  auto missing = config(dir.path() / "c", key);
  missing.custody = source;
  EXPECT_THROW(Node(io.context, missing), std::runtime_error);
}

// Node-local counters beside custody (the maintainer, 2026-11-14): through the Context, an
// advance durable at once and a note durable within the node's next flush; a node
// without custody has none.
TEST(Node, KeepsNodeLocalCountersBesideCustody) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  const auto source = identity::KeySource::typed_passphrase(
      [] {
        const std::string p = "node passphrase";
        return identity::Secret(std::span(reinterpret_cast<const uint8_t*>(p.data()), p.size()));
      },
      identity::Argon2Costs{1024, 1, 1});
  (void)identity::KeyCustody::create(dir.path() / "a", source);
  auto with = config(dir.path() / "a", key);
  with.custody = source;
  // What is on disk, read as a restart would: from another handle on the file.
  identity::KeyCustody custody = identity::KeyCustody::open(dir.path() / "a", source);
  const auto on_disk = [&](std::string_view label) { return identity::SealedCounters(dir.path() / "a", custody.derive("dannan/counter/1")).read(label); };
  {
    Node a(io.context, with);
    a.start();
    auto& identity = a.context().identity();
    EXPECT_EQ(identity.counter("dagda/heads/01/1"), 0u);
    identity.advance_counter("dagda/heads/01/1", 4);
    identity.advance_counters({{"dagda/heads/02/1", 1}, {"lugh/grants/1", 9}});
    EXPECT_THROW(identity.advance_counter("dagda/heads/01/1", 3), std::invalid_argument);
    EXPECT_EQ(on_disk("dagda/heads/01/1"), 4u);
    identity.note_counter("dagda/heads/01/1", 6);
    EXPECT_EQ(identity.counter("dagda/heads/01/1"), 6u);
    // The roster loop's pass, within a second and with the node running, makes the
    // note durable: whether it came before this line or after, the file says so.
    EXPECT_TRUE(eventually([&] { return on_disk("dagda/heads/01/1") == 6u; }, 5s));
    a.stop();
  }
  // What a restart reads.
  EXPECT_EQ(on_disk("dagda/heads/01/1"), 6u);
  EXPECT_EQ(on_disk("lugh/grants/1"), 9u);

  Node b(io.context, config(dir.path() / "b", key));
  EXPECT_THROW((void)b.context().identity().counter("x"), std::logic_error);
  EXPECT_THROW(b.context().identity().note_counter("x", 1), std::logic_error);
}

// A node configured with a certificate: custody made, its id and key
// certified by the cluster's CA.
NodeConfig certified(const std::filesystem::path& state, const identity::CertificateAuthority& ca, std::vector<net::Endpoint> bootstrap,
                     bool also_key = false) {
  const auto source = identity::KeySource::typed_passphrase(
      [] {
        const std::string p = "node";
        return identity::Secret(std::span(reinterpret_cast<const uint8_t*>(p.data()), p.size()));
      },
      identity::Argon2Costs{1024, 1, 1});
  if (!identity::KeyCustody::exists(state)) (void)identity::KeyCustody::create(state, source);
  const auto id = identity::load_or_create_node_id(state);
  const auto key = identity::KeyCustody::open(state, source).node_key().public_key();
  auto out = config(state, identity::ClusterKey{}, std::move(bootstrap));
  out.custody = source;
  out.ca_certificate = ca.certificate();
  out.certificate = ca.issue(id, key, static_cast<uint64_t>(id.bytes[0]) + 1, out.wall->unix_ms() - 1000);
  out.use_cluster_key = also_key;
  return out;
}

// Wire version 2 end to end: nodes holding only certificates form a
// cluster; a node of another CA is never let in; a revocation given to one
// node closes the revoked node out everywhere within a heartbeat, and it
// cannot come back.
TEST(Node, CertificatesFormAClusterAndARevocationClosesANodeOut) {
  Io io;
  test::TempDir dir;
  const auto ca = identity::CertificateAuthority::create(
      "cluster", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 60000);
  Node a(io.context, certified(dir.path() / "a", ca, {}));
  a.start();
  const std::vector<net::Endpoint> seed{{"127.0.0.1", a.port()}};
  Node b(io.context, certified(dir.path() / "b", ca, seed));
  Node c(io.context, certified(dir.path() / "c", ca, seed));
  b.start();
  c.start();
  const auto b_id = b.context().identity().id();
  const auto c_id = c.context().identity().id();
  EXPECT_TRUE(eventually([&] { return knows(a, b_id) && knows(a, c_id) && knows(b, c_id) && knows(c, b_id); }));
  EXPECT_TRUE(b.context().identity().certificate());
  // Another CA's node never gets in.
  const auto stranger_ca = identity::CertificateAuthority::create("cluster", ca.certificate().not_before_unix_ms());
  Node stranger(io.context, certified(dir.path() / "s", stranger_ca, seed));
  stranger.start();
  std::this_thread::sleep_for(500ms);
  EXPECT_FALSE(knows(a, stranger.context().identity().id()));
  stranger.stop();
  // B is revoked, given to A alone; C learns it and both close B out.
  const auto revocation = ca.revoke(b_id, 0, a.context().time().unix_ms(), "decommissioned");
  a.context().identity().revoke(revocation);
  EXPECT_TRUE(eventually([&] { return c.context().identity().revocations().size() == 1; }));
  const auto c_record = c.context().nodes().self();
  EXPECT_TRUE(eventually([&] { return !b.context().routes().call_and_wait(c_record, net::message::ping, {}, FrameType::control, 1s).reply; }));
  const auto a_record = a.context().nodes().self();
  EXPECT_FALSE(b.context().routes().call_and_wait(a_record, net::message::ping, {}, FrameType::control, 1s).reply);
  // Another CA's revocation is refused, never kept or passed on.
  EXPECT_THROW(a.context().identity().revoke(stranger_ca.revoke(c_id, 0, 0, "not ours")), std::invalid_argument);
  EXPECT_EQ(a.context().identity().revocations().size(), 1u);
  c.stop();
  b.stop();
  a.stop();
}

// A peer's certificate, as it proved it, and the cluster's CA reach the
// hosted system: what verifies a peer's signature (Dagda asked,
// 2026-11-04). None for a node never met by certificate, or one revoked.
TEST(Node, APeersCertificateAndTheCaAreGiven) {
  Io io;
  test::TempDir dir;
  const auto ca = identity::CertificateAuthority::create(
      "cluster", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - 60000);
  Node a(io.context, certified(dir.path() / "a", ca, {}));
  a.start();
  Node b(io.context, certified(dir.path() / "b", ca, {{"127.0.0.1", a.port()}}));
  b.start();
  const auto b_id = b.context().identity().id();
  ASSERT_TRUE(eventually([&] { return a.context().nodes().certificate(b_id).has_value(); }));
  EXPECT_EQ(a.context().nodes().certificate(b_id)->fingerprint(), b.context().identity().certificate()->fingerprint());
  ASSERT_TRUE(a.context().identity().ca_certificate());
  EXPECT_EQ(a.context().identity().ca_certificate()->fingerprint(), ca.certificate().fingerprint());
  // Its key verifies what B signed.
  const net::Bytes message{4, 5, 6};
  const auto key = a.context().nodes().certificate(b_id)->public_key();
  EXPECT_TRUE(identity::verify(*identity::algorithm_of_public_key(key), key, message, b.context().identity().sign(message)));
  EXPECT_FALSE(a.context().nodes().certificate(identity::NodeId{}));
  a.context().identity().revoke(ca.revoke(b_id, 0, a.context().time().unix_ms(), "retired"));
  EXPECT_FALSE(a.context().nodes().certificate(b_id));
  b.stop();
  a.stop();
  // A cluster-key node holds neither.
  Pair pair;
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return knows(*pair.a, pair.b->context().identity().id()); }));
  EXPECT_FALSE(pair.a->context().nodes().certificate(pair.b->context().identity().id()));
  EXPECT_FALSE(pair.a->context().identity().ca_certificate());
}

// A node refuses to start on a certificate that is not its own: another
// node's id, another key, or none of the CA's.
TEST(Node, RefusesACertificateThatIsNotItsOwn) {
  Io io;
  test::TempDir dir;
  const auto now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
  const auto ca = identity::CertificateAuthority::create("cluster", now_ms - 60000);
  auto mine = certified(dir.path() / "a", ca, {});
  auto theirs = certified(dir.path() / "b", ca, {});
  auto wrong_node = mine;
  wrong_node.certificate = theirs.certificate;
  EXPECT_THROW(Node(io.context, wrong_node), std::invalid_argument);
  auto wrong_key = mine;
  wrong_key.certificate = ca.issue(identity::load_or_create_node_id(dir.path() / "a"),
                                   identity::SigningKey::generate(identity::SigningAlgorithm::es256).public_key(), 99, now_ms - 1000);
  EXPECT_THROW(Node(io.context, wrong_key), std::invalid_argument);
  auto no_ca = mine;
  no_ca.ca_certificate.reset();
  EXPECT_THROW(Node(io.context, no_ca), std::invalid_argument);
  EXPECT_NO_THROW(Node(io.context, mine));
}

// Scope: without a source only the cluster's nodes may write; the source
// the directory installs then answers, with its evidence.
TEST(Node, ChecksScopeThroughTheInstalledSource) {
  Pair pair;
  pair.b->start();
  const auto b = pair.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.a, b); }));
  auto& identity = pair.a->context().identity();
  using contract::Principal;
  EXPECT_TRUE(identity.verify({Principal::Kind::node, identity::to_string(identity.id())}, "lugh/scope").allowed);
  EXPECT_TRUE(identity.verify({Principal::Kind::node, identity::to_string(b)}, "lugh/scope").allowed);
  EXPECT_FALSE(identity.verify({Principal::Kind::node, identity::to_string(identity::NodeId::random())}, "x").allowed);
  const auto refused = identity.verify({Principal::Kind::directory, "alice"}, "lugh/scope");
  EXPECT_FALSE(refused.allowed);
  EXPECT_NE(refused.why.find("no scope source"), std::string::npos);

  struct Directory final : contract::ScopeSource {
    contract::ScopeDecision may_write(const contract::Principal& principal, std::string_view prefix) const override {
      const bool allowed = principal.id == "alice" && prefix.rfind("macha/alice/", 0) == 0;
      return {allowed, 42, allowed ? std::string() : "outside alice's scope"};
    }
  };
  identity.install_scope_source(std::make_shared<Directory>());
  const auto granted = identity.verify({Principal::Kind::directory, "alice"}, "macha/alice/photos");
  EXPECT_TRUE(granted.allowed);
  EXPECT_EQ(granted.generation, 42u);
  EXPECT_FALSE(identity.verify({Principal::Kind::directory, "alice"}, "macha/bob/").allowed);
  EXPECT_FALSE(identity.verify({Principal::Kind::directory, "mallory"}, "macha/alice/").allowed);
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
  ASSERT_TRUE(outcome.peer);
  EXPECT_EQ(outcome.peer->id, a.id);
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
  // Membership moved when A joined, and an advert does not move it.
  // Known a moment before the event: wait for it.
  ASSERT_TRUE(eventually([&] { return pair.b->context().events().count(status::NodeEvents::membership) > 0; }));
  const auto members = pair.b->context().events().count(status::NodeEvents::membership);
  pair.a->context().nodes().advertise("capacity", membership::u64_advert(4096));
  const auto advertised = [&] {
    for (const auto& node : pair.b->context().nodes().active())
      if (node.id == a) return node.advert_u64("capacity") == 4096u;
    return false;
  };
  EXPECT_TRUE(eventually(advertised));
  EXPECT_TRUE(eventually([&] { return pair.b->context().events().count(status::NodeEvents::topology) > topology; }));
  EXPECT_EQ(pair.b->context().events().count(status::NodeEvents::membership), members);
  const placement::Scheme scheme{"test/fallback", "test/extra-domain", "capacity"};
  const std::vector<uint8_t> key(32, 7);
  const auto placed = pair.b->context().nodes().placement(key, 1, scheme, "capacity");
  ASSERT_EQ(placed.size(), 1u);
  EXPECT_EQ(placed.front().id, a);
  EXPECT_EQ(pair.b->context().nodes().placement(key, 2, scheme).size(), 2u);
  // The node's placement is the pure function's over its roster, narrowed as
  // with_advert narrows it, whatever the node copies or does not on the way
  // (Dagda's measurement: one call copied every record about five times). The
  // roster is read apart from each placement, which takes its own; placement
  // reads only ids, failure domains, path classes and the capacity advert, all
  // fixed by now, so a heartbeat between the two changes nothing (Dagda's review).
  const auto ids = [](const std::vector<membership::NodeRecord>& nodes) {
    std::vector<identity::NodeId> out;
    for (const auto& node : nodes) out.push_back(node.id);
    return out;
  };
  for (uint8_t seed = 0; seed < 16; ++seed) {
    const std::vector<uint8_t> each(32, seed);
    for (const size_t copies : {size_t{1}, size_t{2}, size_t{3}}) {
      const auto roster = pair.b->context().nodes().active();
      EXPECT_EQ(ids(pair.b->context().nodes().placement(each, copies, scheme)), ids(placement::by_capacity(each, roster, copies, scheme)));
      EXPECT_EQ(ids(pair.b->context().nodes().placement(each, copies, scheme, "capacity")),
                ids(placement::by_capacity(each, placement::with_advert(roster, "capacity"), copies, scheme)));
    }
  }
  EXPECT_THROW(pair.b->context().nodes().placement(key, 1, placement::Scheme{}), std::invalid_argument);
  pair.a->context().nodes().withdraw("capacity");
  EXPECT_TRUE(eventually([&] { return pair.b->context().nodes().placement(key, 1, scheme, "capacity").empty(); }));
  EXPECT_THROW(pair.a->context().nodes().advertise("", {}), std::invalid_argument);
}

// A watch on one advert (P6): the value now first, then each change of that
// key and no other, a peer's at most once a period, the changes inside it
// coalesced to the latest; a withdrawal is told as no value, and an unwatched
// watch hears nothing.
TEST(Node, AnAdvertWatchTellsOneKeysChangesAtItsRate) {
  struct Told {
    identity::NodeId node;
    std::optional<uint64_t> value;
    std::chrono::steady_clock::time_point at;
  };
  std::mutex mutex;
  std::vector<Told> told;
  const auto count = [&] {
    std::lock_guard lock(mutex);
    return told.size();
  };
  const auto last = [&] {
    std::lock_guard lock(mutex);
    return told.back();
  };
  Pair pair;
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  pair.a->context().nodes().advertise("used", membership::u64_advert(1));
  ASSERT_TRUE(eventually([&] {
    const auto record = record_of(*pair.b, a);
    return record && record->advert_u64("used") == 1u;
  }));
  const auto watch = pair.b->context().nodes().watch_advert("used", 1500ms, asio::make_strand(pair.io.context), [&](AdvertChange change) {
    std::optional<uint64_t> value;
    if (change.value) {
      membership::NodeRecord carrier;
      carrier.adverts["used"] = *change.value;
      value = carrier.advert_u64("used");
    }
    std::lock_guard lock(mutex);
    told.push_back({change.node, value, std::chrono::steady_clock::now()});
  });
  ASSERT_TRUE(eventually([&] { return count() == 1; }));
  EXPECT_EQ(last().node, a);
  EXPECT_EQ(last().value, 1u);

  // Another key is not this watch's.
  pair.a->context().nodes().advertise("capacity", membership::u64_advert(4096));
  pair.a->context().nodes().advertise("used", membership::u64_advert(2));
  ASSERT_TRUE(eventually([&] { return count() == 2; }));
  EXPECT_EQ(last().value, 2u);
  const auto second = last().at;
  // Inside the period: held back, then only the latest.
  pair.a->context().nodes().advertise("used", membership::u64_advert(3));
  pair.a->context().nodes().advertise("used", membership::u64_advert(4));
  ASSERT_TRUE(eventually([&] { return last().value == 4u; }));
  EXPECT_EQ(count(), 3u);
  EXPECT_GE(last().at - second, 1450ms);

  // A withdrawal inside the period is told at once, never held back. Gossip
  // reaches B within a heartbeat or two (100 ms); one held back would come
  // only at the period's end, some 1400 ms from here.
  const auto withdrawn = std::chrono::steady_clock::now();
  pair.a->context().nodes().withdraw("used");
  ASSERT_TRUE(eventually([&] { return count() == 4; }));
  EXPECT_FALSE(last().value);
  EXPECT_LT(last().at - withdrawn, 1200ms);

  pair.b->context().nodes().unwatch_advert(watch);
  pair.a->context().nodes().advertise("used", membership::u64_advert(5));
  EXPECT_TRUE(eventually([&] {
    const auto record = record_of(*pair.b, a);
    return record && record->advert_u64("used") == 5u;
  }));
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(count(), 4u);
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

// Which side of a split this node is on, against the roster as last agreed
// (P2): a node that leaves stays counted, so one node of two is a minority
// (an even split); only an operator's retire takes it out of the count. A
// watcher hears each change.
TEST(Node, APartitionIsReadAgainstTheAgreedRoster) {
  // Before the pair: a change posted while the nodes stop still finds them.
  std::mutex mutex;
  std::vector<membership::Partition> heard;
  Pair pair;
  const auto strand = asio::make_strand(pair.io.context);
  const auto watching = pair.a->context().nodes().watch_partition(strand, [&](membership::Partition side) {
    std::lock_guard lock(mutex);
    heard.push_back(std::move(side));
  });
  const auto heard_minority = [&] {
    std::lock_guard lock(mutex);
    return std::any_of(heard.begin(), heard.end(), [](const auto& side) { return !side.majority && side.members == 2; });
  };
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  const auto b = pair.b->context().identity().id();
  auto& nodes = pair.a->context().nodes();
  ASSERT_TRUE(eventually([&] {
    const auto side = nodes.partition();
    return side.members == 2 && side.reachable == 2 && side.majority;
  }));
  EXPECT_EQ(nodes.members(), (std::set<identity::NodeId>{a, b}));
  pair.b->stop();
  ASSERT_TRUE(eventually([&] { return nodes.partition().reachable == 1; }));
  const auto alone = nodes.partition();
  EXPECT_FALSE(alone.majority) << "one of two is an even split, a minority";
  EXPECT_EQ(alone.members, 2u);
  EXPECT_EQ(alone.unreachable, (std::vector<identity::NodeId>{b}));
  EXPECT_TRUE(eventually(heard_minority));
  EXPECT_THROW(nodes.retire(a), std::invalid_argument);
  EXPECT_TRUE(nodes.retire(b));
  EXPECT_FALSE(nodes.retire(b));
  const auto retired = nodes.partition();
  EXPECT_TRUE(retired.majority);
  EXPECT_EQ(retired.members, 1u);
  EXPECT_EQ(nodes.members(), (std::set<identity::NodeId>{a}));
  nodes.unwatch_partition(watching);
}

// The roster's changes as a feed: a watcher hears a node join, change (an
// advert; a restart is a new incarnation), and leave once it has been
// silent for dead_after.
TEST(Node, AWatcherHearsTheRosterChange) {
  Pair pair;
  std::mutex mutex;
  std::vector<RosterChange> heard;
  const auto strand = asio::make_strand(pair.io.context);
  pair.a->context().nodes().watch(strand, [&](RosterChange change) {
    std::lock_guard lock(mutex);
    heard.push_back(std::move(change));
  });
  const auto has = [&](RosterChange::Kind kind, const std::function<bool(const membership::NodeRecord&)>& also = {}) {
    std::lock_guard lock(mutex);
    return std::any_of(heard.begin(), heard.end(), [&](const RosterChange& c) { return c.kind == kind && (!also || also(c.node)); });
  };
  pair.b->start();
  const auto b = pair.b->context().identity().id();
  const auto epoch = pair.b->context().identity().durability_epoch();
  ASSERT_TRUE(eventually([&] { return has(RosterChange::Kind::joined, [&](const auto& n) { return n.id == b; }); }));
  EXPECT_TRUE(has(RosterChange::Kind::joined, [&](const auto& n) { return n.id == b && n.incarnation == epoch; }));
  pair.b->context().nodes().advertise("role", net::Bytes{1});
  EXPECT_TRUE(eventually([&] { return has(RosterChange::Kind::changed, [&](const auto& n) { return n.id == b && n.advert("role"); }); }));
  pair.b->stop();
  EXPECT_TRUE(eventually([&] { return has(RosterChange::Kind::left, [&](const auto& n) { return n.id == b; }); }));
  // A watcher that comes later is told of the roster as it stands.
  pair.a->context().nodes().watch(strand, [&](RosterChange) {});
  EXPECT_STREQ(roster_change_name(RosterChange::Kind::left), "left");
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

// The node's own lines, level and words (first cutover, section 5).
TEST(Journal, AnInboundChangeAndItsProbesAreSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  auto hub_log = std::make_shared<test::RecordingSink>();
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  auto hub_config = config(dir.path() / "a", key);
  hub_config.log = hub_log;
  Node hub(io.context, hub_config);
  hub.start();
  auto settings = config(dir.path() / "b", key, {{"127.0.0.1", hub.port()}});
  settings.log = log;
  settings.inbound = reach::Inbound::automatic;
  settings.inbound_journal_fields = [](bool capable) { return std::string(" hosts_extents=") + (capable ? "true" : "false"); };
  Node site(io.context, settings);
  site.start();
  EXPECT_TRUE(eventually([&] {
    return log->count(status::LogLevel::info, "node inbound resolution changed inbound_capable=true hosts_extents=true previous_inbound_capable=") >= 1;
  }));
  const auto hub_id = identity::to_string(hub.context().identity().id()).substr(0, 12);
  EXPECT_GE(log->count(status::LogLevel::debug, "dial-back probe peer=" + hub_id + " endpoint="), 1u);
  EXPECT_GE(log->count(status::LogLevel::debug, " reachable=true consecutive_failures=0"), 1u);
  const auto site_id = identity::to_string(site.context().identity().id()).substr(0, 12);
  EXPECT_GE(hub_log->count(status::LogLevel::debug, "dial-back probe for peer=" + site_id + " endpoint="), 1u);
  site.stop();
  hub.stop();
}

TEST(Journal, AnInboundFileThatCannotBeWrittenIsWarnedOf) {
  auto log = std::make_shared<test::RecordingSink>();
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  Node hub(io.context, config(dir.path() / "a", key));
  hub.start();
  // A directory where the file belongs.
  std::filesystem::create_directories(dir.path() / "b" / "connectivity" / "inbound.bin" / "held");
  auto settings = config(dir.path() / "b", key, {{"127.0.0.1", hub.port()}});
  settings.log = log;
  settings.inbound = reach::Inbound::automatic;
  Node site(io.context, settings);
  site.start();
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::warn, "inbound resolution not persisted: ") >= 1; }));
  site.stop();
  hub.stop();
}

// A peer that answers with anything but its verdict: here, past its rate limit.
TEST(Journal, ADialBackNobodyAnswersIsSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  auto hub_config = config(dir.path() / "a", key);
  hub_config.dial_back_min_interval = std::chrono::hours(1);
  Node hub(io.context, hub_config);
  hub.start();
  auto settings = config(dir.path() / "b", key, {{"127.0.0.1", hub.port()}});
  settings.log = log;
  settings.inbound = reach::Inbound::automatic;
  // Asks again at once: the first is answered, the next is past the limit.
  settings.reprobe_while_capable = 100ms;
  settings.reprobe_while_incapable = 100ms;
  Node site(io.context, settings);
  site.start();
  const auto hub_id = identity::to_string(hub.context().identity().id()).substr(0, 12);
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "dial-back probe not answered peer=" + hub_id + " error=") >= 1; }));
  site.stop();
  hub.stop();
}

TEST(Journal, StoppingTheLoopsIsSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  Pair pair([&](NodeConfig& c) { c.log = log; });
  pair.b->start();
  pair.b->stop();
  EXPECT_EQ(log->count(status::LogLevel::debug, "shutdown: node maintenance request_stop"), 1u);
}

TEST(Journal, ATelemetrySampleThatFailsIsSaidAndTheLoopGoesOn) {
  auto log = std::make_shared<test::RecordingSink>();
  Pair pair([&](NodeConfig& c) {
    c.log = log;
    c.telemetry_fields = [](status::Telemetry&) { throw std::runtime_error("sampling broke"); };
  });
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "telemetry refresh skipped: sampling broke") >= 2; }));
}

TEST(Journal, AnIdentityResetThatCannotReachAPeerIsSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  auto a_config = config(dir.path() / "a", key);
  a_config.log = log;
  Node a(io.context, a_config);
  a.start();
  Node b(io.context, config(dir.path() / "b", key, {{"127.0.0.1", a.port()}}));
  b.start();
  ASSERT_TRUE(eventually([&] { return knows(a, b.context().identity().id()); }));
  const auto host = a.membership().active().front().host;
  b.stop();
  membership::IdentityReset reset;
  reset.host = "10.9.9.9";
  reset.port = 7437;
  reset.epoch = 1;
  reset.reset_unix_ms = 1;
  reset.reason = "disk replaced";
  a.propagate_identity_reset(reset, 500ms);
  EXPECT_GE(log->count(status::LogLevel::debug, "identity reset propagation to " + host + ": "), 1u);
  a.stop();
}

TEST(Journal, AnEndpointThatChangesIsSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  Pair pair([&](NodeConfig& c) {
    c.log = log;
    c.connectivity.external_ip.enabled = true;
    c.external_ip = [](std::chrono::milliseconds) { return std::string("203.0.113.7"); };
  });
  pair.b->start();
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::info, "node advertised endpoint changed from=127.0.0.1:") == 1; }));
  EXPECT_EQ(log->count(status::LogLevel::info, " to=203.0.113.7:"), 1u);
}

TEST(Journal, ANodeForgottenAtTheHorizonIsSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  auto a_config = config(dir.path() / "a", key);
  a_config.log = log;
  a_config.dead_after = 200ms;
  a_config.forget_after = 300ms;
  Node a(io.context, a_config);
  a.start();
  Node b(io.context, config(dir.path() / "b", key, {{"127.0.0.1", a.port()}}));
  b.start();
  ASSERT_TRUE(eventually([&] { return knows(a, b.context().identity().id()); }));
  b.stop();
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::info, "membership: forgot 1 node(s) not heard of for the absence horizon") == 1; }));
  a.stop();
}

// Macha's defaults where Macha has them; the four no-progress settings are
// zero, by ruling A: every call states its own.
TEST(NodeConfig, TheDefaultsAreMachasOrRuled) {
  const NodeConfig defaults;
  EXPECT_EQ(defaults.control_no_progress.count(), 0);
  EXPECT_EQ(defaults.data_no_progress.count(), 0);
  EXPECT_EQ(defaults.control_stall_notice.count(), 0);
  EXPECT_EQ(defaults.data_stall_notice.count(), 0);
  EXPECT_EQ(defaults.exchange_budget, 30s);
  EXPECT_EQ(defaults.dial_back_min_interval, 10s);
  EXPECT_EQ(defaults.reprobe_while_incapable, 10min);
  EXPECT_EQ(defaults.reprobe_while_capable, 1h);
}

// cluster.cpp:38: a node with no failure domain of its own is its host's.
TEST(Node, AnEmptyFailureDomainIsTheHost) {
  Pair pair;
  EXPECT_EQ(pair.a->membership().self().failure_domain, "127.0.0.1");
  Pair named([](NodeConfig& c) { c.failure_domain = "rack-2"; });
  EXPECT_EQ(named.b->membership().self().failure_domain, "rack-2");
}

// A seed, not a master: it goes, and those who met through it carry on.
TEST(Node, TheOthersCarryOnWithoutTheSeed) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  auto seed = std::make_unique<Node>(io.context, config(dir.path() / "s", key));
  seed->start();
  const std::vector<net::Endpoint> bootstrap{{"127.0.0.1", seed->port()}};
  Node b(io.context, config(dir.path() / "b", key, bootstrap));
  Node c(io.context, config(dir.path() / "c", key, bootstrap));
  b.start();
  c.start();
  const auto b_id = b.context().identity().id();
  const auto c_id = c.context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(b, c_id) && knows(c, b_id); }));
  seed->stop();
  seed.reset();
  std::this_thread::sleep_for(3s);  // past dead_after
  EXPECT_TRUE(knows(b, c_id));
  EXPECT_TRUE(knows(c, b_id));
  std::optional<membership::NodeRecord> peer;
  for (const auto& node : b.context().nodes().active())
    if (node.id == c_id) peer = node;
  ASSERT_TRUE(peer.has_value());
  EXPECT_TRUE(b.context().routes().call_and_wait(*peer, net::message::members, {}, sched::FrameType::control, 5s).reply);
  c.stop();
  b.stop();
}

// The node's hybrid clock (the HLC design): on the Context, its ceiling in a durable file when the
// node has no key custody, its counts on Diagnostics, and every stamp after a restart later than
// every stamp before, the wall set back between.
TEST(Node, ItsHybridClockIssuesLaterStampsAfterARestartWithTheWallSetBack) {
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  test::SteppedWall wall(1'790'000'000'000);
  contract::HlcStamp before;
  {
    Io io;
    auto settings = config(dir.path() / "a", key);
    settings.wall = &wall;
    Node node(io.context, settings);
    for (int i = 0; i < 100; ++i) before = node.context().hlc().now();
    const auto far = node.context().hlc().receive(contract::HlcStamp::of(wall.unix_ms() + 120'000, 0), "far peer");
    EXPECT_FALSE(far.merged);
    EXPECT_EQ(node.context().diagnostics().hlc.not_merged, 1u);
    EXPECT_EQ(node.context().diagnostics().hlc.peers.count("far peer"), 1u);
    EXPECT_TRUE(std::filesystem::exists(dir.path() / "a" / "clock" / "hlc-ceiling"));
  }
  wall.advance(-60s);
  Io io;
  auto settings = config(dir.path() / "a", key);
  settings.wall = &wall;
  Node node(io.context, settings);
  EXPECT_GT(node.context().hlc().now(), before);
}

// With key custody the ceiling is a sealed counter, "dannan/hlc", and no plain file is written.
TEST(Node, ItsHybridClocksCeilingIsASealedCounterWithCustody) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  const auto source = identity::KeySource::typed_passphrase(
      [] {
        const std::string p = "node passphrase";
        return identity::Secret(std::span(reinterpret_cast<const uint8_t*>(p.data()), p.size()));
      },
      identity::Argon2Costs{1024, 1, 1});
  (void)identity::KeyCustody::create(dir.path() / "a", source);
  auto with = config(dir.path() / "a", key);
  with.custody = source;
  Node node(io.context, with);
  const auto stamp = node.context().hlc().now();
  EXPECT_GT(node.context().identity().counter("dannan/hlc"), stamp.unix_ms());
  EXPECT_FALSE(std::filesystem::exists(dir.path() / "a" / "clock" / "hlc-ceiling"));
}

// The loop re-probes on its cadence, and a capability it resolves reaches a
// peer by gossip.
TEST(Node, AnAutomaticNodeReprobesAndItsCapabilityReachesAPeer) {
  auto log = std::make_shared<test::RecordingSink>();
  Pair pair([&](NodeConfig& c) {
    c.log = log;
    c.inbound = reach::Inbound::automatic;
    c.reprobe_while_capable = 150ms;
    c.reprobe_while_incapable = 150ms;
  });
  pair.b->start();
  const auto b_id = pair.b->context().identity().id();
  EXPECT_TRUE(eventually([&] { return log->count(status::LogLevel::debug, "dial-back probe peer=") >= 1; }));
  EXPECT_TRUE(eventually([&] {
    for (const auto& node : pair.a->context().nodes().active())
      if (node.id == b_id && node.inbound_capable()) return true;
    return false;
  }));
}

// One traffic instance: a node's accepted sessions and its dialled ones count
// in the same place.
TEST(Node, ItsDialledAndAcceptedSessionsCountInOneInstance) {
  constexpr net::MessageType echo_data{4243};
  const auto answer = [](const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
    return net::Handled::processed({net::message::ok, request.payload, {}});
  };
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  Node a(io.context, config(dir.path() / "a", key));
  a.context().routes().add(echo_data, {"echo_data", net::Lanes::data, net::Frames::not_control, false}, answer);
  a.start();
  Node b(io.context, config(dir.path() / "b", key, {{"127.0.0.1", a.port()}}));
  b.context().routes().add(echo_data, {"echo_data", net::Lanes::data, net::Frames::not_control, false}, answer);
  b.start();
  const auto b_id = b.context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(a, b_id); }));
  std::optional<membership::NodeRecord> peer;
  for (const auto& node : a.context().nodes().active())
    if (node.id == b_id) peer = node;
  ASSERT_TRUE(peer.has_value());
  const auto control_class = static_cast<size_t>(sched::FrameType::control);
  const auto foreground_class = static_cast<size_t>(sched::FrameType::foreground);
  // CONTROL arrived on the session B dialled; A dials DATA itself for this call.
  ASSERT_TRUE(eventually([&] { return a.traffic().in_bytes[control_class] > 0; }));
  EXPECT_EQ(a.traffic().in_bytes[foreground_class], 0u);
  ASSERT_TRUE(a.context().routes().call_and_wait(*peer, echo_data, net::Bytes(5000, 1), sched::FrameType::foreground, 10s).reply);
  EXPECT_TRUE(eventually([&] { return a.traffic().in_bytes[foreground_class] >= 5000; }));
  EXPECT_GT(a.traffic().in_bytes[control_class], 0u);
  b.stop();
  a.stop();
}

TEST(StateLock, ASecondNodeOnOneStatePathIsRefused) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  Node first(io.context, config(dir.path() / "a", key));
  // A file a second node would read and complain of, had it got that far.
  std::filesystem::create_directories(dir.path() / "a" / "telemetry");
  std::ofstream(dir.path() / "a" / "telemetry" / "last-known.bin") << "not this";
  auto settings = config(dir.path() / "a", key);
  auto log = std::make_shared<test::RecordingSink>();
  settings.log = log;
  EXPECT_THROW(Node second(io.context, settings), std::exception);
  EXPECT_EQ(log->count(status::LogLevel::warn, "persisted telemetry ignored"), 0u);
}

// The node's timer and a call's deadline follow its injected time, so a
// hosted system testing on stepped time can move them.
TEST(Time, AfterAndACallsDeadlineFollowTheInjectedTime) {
  constexpr net::MessageType held{4244};
  test::Gate gate;
  test::GateOpener opener(gate);
  test::SteppedTime time;
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  auto a_config = config(dir.path() / "a", key);
  a_config.time = &time;
  Node a(io.context, a_config);
  a.start();
  Node b(io.context, config(dir.path() / "b", key));
  b.context().routes().add(held, {"held", net::Lanes::control, net::Frames::control_only, false},
                           [&](const membership::NodeRecord&, sched::FrameType, const net::Message&) {
                             gate.enter_and_wait();
                             return net::Handled::processed({net::message::ok, {}, {}});
                           });
  b.start();
  std::atomic<bool> fired{false};
  a.context().time().after(1h, [&] { fired = true; });
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(fired.load());
  std::promise<net::CallOutcome> answered;
  auto future = answered.get_future();
  a.context().routes().call(b.context().nodes().self(), held, {}, sched::FrameType::control, 2h, io.context.get_executor(),
                            [&](net::CallOutcome outcome) { answered.set_value(std::move(outcome)); });
  ASSERT_TRUE(gate.wait_for_entries(1));
  EXPECT_EQ(future.wait_for(50ms), std::future_status::timeout);
  time.advance(1h);
  EXPECT_TRUE(eventually([&] { return fired.load(); }));
  EXPECT_EQ(future.wait_for(50ms), std::future_status::timeout);
  time.advance(1h);
  ASSERT_EQ(future.wait_for(10s), std::future_status::ready);
  const auto outcome = future.get();
  EXPECT_FALSE(outcome.reply);
  EXPECT_NE(outcome.failure.find("deadline of 7200000 ms passed"), std::string::npos) << outcome.failure;
  gate.open();
  b.stop();
  a.stop();
}

// Law 1 and a clean stop: a peer learnt on an io thread reaches the disk by the
// roster thread, and a disk that refuses costs a recorded fault, not the transport.
TEST(Node, TheRosterIsWrittenOffTheIoThreadsAndAtStop) {
  Pair pair;
  pair.b->start();
  const auto b_id = pair.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.a, b_id); }));
  const auto path = pair.dir.path() / "a" / "membership" / "known-nodes.bin";
  EXPECT_TRUE(eventually([&] { return std::filesystem::exists(path); }));
  const auto names = [&] {
    std::set<std::string> out;
    for (const auto& status : pair.a->diagnostics().threads) out.insert(status.name);
    return out;
  };
  EXPECT_TRUE(eventually([&] { return names().contains("cluster-roster"); }));
  pair.b->stop();
  pair.a->stop();
  membership::Membership reloaded(membership::NodeRecord{}, 2s, path);
  const auto all = reloaded.all();
  EXPECT_TRUE(std::any_of(all.begin(), all.end(), [&](const membership::NodeRecord& node) { return node.id == b_id; }));
}

TEST(Node, ARosterThatCannotBeWrittenIsAFaultAndTheNodeGoesOn) {
  test::TempDir dir;
  Io io;
  const auto key = cluster_key(dir.path());
  Node a(io.context, config(dir.path() / "a", key));
  a.start();
  const auto path = dir.path() / "a" / "membership" / "known-nodes.bin";
  std::filesystem::remove_all(path);
  std::filesystem::create_directories(path / "in-the-way");
  Node b(io.context, config(dir.path() / "b", key, {{"127.0.0.1", a.port()}}));
  b.start();
  const auto b_id = b.context().identity().id();
  EXPECT_TRUE(eventually([&] { return knows(a, b_id); }));
  EXPECT_TRUE(eventually([&] {
    for (const auto& status : a.diagnostics().threads)
      if (status.name == "cluster-roster" && status.faults > 0) return true;
    return false;
  }));
  std::filesystem::remove_all(path);
  b.stop();
  a.stop();
}

// An authenticated peer is on the roster before its first request can be read, on a
// session it dialled: there is no window for "not a node of this cluster".
TEST(Node, APeersFirstRequestFindsItOnTheRoster) {
  constexpr net::MessageType ask{4245};
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  std::atomic<int> refused{0};
  std::atomic<int> allowed{0};
  Node a(io.context, config(dir.path() / "a", key));
  a.context().routes().add(ask, {"ask", net::Lanes::control, net::Frames::any, false},
                           [&](const membership::NodeRecord& peer, sched::FrameType, const net::Message&) {
                             contract::Principal principal;
                             principal.kind = contract::Principal::Kind::node;
                             principal.id = identity::to_string(peer.id);
                             ++(a.context().identity().verify(principal, "anything").allowed ? allowed : refused);
                             return net::Handled::processed({net::message::ok, {}, {}});
                           });
  a.start();
  // Knows no seed, so nothing has told A of it but its own connection.
  Node b(io.context, config(dir.path() / "b", key));
  b.start();
  const auto outcome = b.context().routes().call_and_wait(net::Endpoint{"127.0.0.1", a.port()}, ask, {}, sched::FrameType::control, 5s);
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(allowed.load(), 1);
  EXPECT_EQ(refused.load(), 0);
  b.stop();
  a.stop();
}

// A node stopped and destroyed while its io context runs on, with a peer still
// sending: nothing read before the stop reaches it afterwards. Dagda's in-process
// restart, under its delete stress.
TEST(Node, ARestartOnASharedIoContextLeavesNothingBehind) {
  constexpr net::MessageType news{4246};
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  Node b(io.context, config(dir.path() / "b", key));
  b.context().routes().add(news, {"news", net::Lanes::control, net::Frames::any, true},
                           [](const membership::NodeRecord&, sched::FrameType, const net::Message&) { return net::Handled::processed(); });
  b.start();
  for (int round = 0; round < 3; ++round) {
    auto a = std::make_unique<Node>(io.context, config(dir.path() / "a", key, {{"127.0.0.1", b.port()}}));
    a->context().routes().add(news, {"news", net::Lanes::control, net::Frames::any, true},
                              [](const membership::NodeRecord&, sched::FrameType, const net::Message&) { return net::Handled::processed(); });
    a->start();
    const auto a_id = a->context().identity().id();
    ASSERT_TRUE(eventually([&] { return knows(b, a_id); }));
    std::atomic<bool> sending{true};
    std::thread flood([&] {
      std::optional<membership::NodeRecord> to;
      for (const auto& node : b.context().nodes().active())
        if (node.id == a_id) to = node;
      if (!to) return;
      std::atomic<int> outstanding{0};
      while (sending) {
        if (outstanding.load() > 64) {
          std::this_thread::yield();
          continue;
        }
        ++outstanding;
        (void)b.context().routes().call(*to, news, net::Bytes(4096, 1), sched::FrameType::control, 2s, io.context.get_executor(),
                                        [&outstanding](net::CallOutcome) { --outstanding; });
      }
      while (outstanding.load() > 0) std::this_thread::sleep_for(10ms);
    });
    std::this_thread::sleep_for(100ms);
    a->stop();
    a.reset();
    std::this_thread::sleep_for(50ms);
    sending = false;
    flood.join();
  }
  b.stop();
}

// An acceptor store as a hosted system would keep one in memory.
class MemoryStore final : public agree::AcceptorStore {
 public:
  void load(agree::Slot slot, std::span<const agree::Name> names, bool every, bool values, std::function<void(agree::Held)> done) override {
    agree::Held out;
    {
      std::lock_guard lock(_mutex);
      out.slot = _slots[slot];
      out.floor = _floor;
      for (const auto& name : names) {
        out.names.push_back(name);
        out.states.push_back(_held[{slot, std::string(name.begin(), name.end())}]);
      }
      if (every)
        for (const auto& [key, state] : _held) {
          const agree::Name name(key.second.begin(), key.second.end());
          if (key.first != slot || std::find(names.begin(), names.end(), name) != names.end()) continue;
          out.names.push_back(name);
          out.states.push_back(state);
        }
    }
    if (!values)
      for (auto& state : out.states) state.value.reset();
    done(std::move(out));
  }
  // Takes no deltas: every one is asked for whole, and nothing kept.
  void keep(agree::Slot slot, const agree::Held& held, std::function<void(Kept)> done) override {
    Kept out;
    for (size_t i = 0; i < held.states.size(); ++i)
      if (held.states[i].delta) out.need_whole.push_back(i);
    if (!out.need_whole.empty()) return done(std::move(out));
    {
      std::lock_guard lock(_mutex);
      _slots[slot] = held.slot;
      _floor = held.floor;
      for (size_t i = 0; i < held.names.size(); ++i) {
        auto& kept = _held[{slot, std::string(held.names[i].begin(), held.names[i].end())}];
        auto state = held.states[i];
        // Held, not carried: the value stays as it is.
        if (state.has_value && !state.value) state.value = kept.value;
        kept = std::move(state);
      }
    }
    out.kept = true;
    done(std::move(out));
  }
  bool ready() const override { return true; }

 private:
  std::mutex _mutex;
  // Keyed by the name's bytes as a string: GCC 14 at -O3 misreads a map of byte vectors.
  std::map<std::pair<agree::Slot, std::string>, agree::AcceptorState> _held;
  std::map<agree::Slot, agree::SlotState> _slots;
  agree::Ballot _floor;
};

const agree::Slot k_slot = agree::slot_of(agree::Name{'k'}, agree::default_slots);

agree::Outcome told_within(const std::function<void(std::function<void(agree::Outcome)>)>& start) {
  auto told = std::make_shared<std::promise<agree::Outcome>>();
  start([told](agree::Outcome outcome) { told->set_value(std::move(outcome)); });
  auto future = told->get_future();
  if (future.wait_for(30s) != std::future_status::ready) return {};
  return future.get();
}

agree::Outcome agree_now(Node& node, const std::vector<identity::NodeId>& replicas, uint64_t version, agree::Change change, std::chrono::milliseconds deadline,
                         Io& io) {
  return told_within([&](std::function<void(agree::Outcome)> done) {
    node.context().agreement().change("test", k_slot, {agree::Name{'k'}}, replicas, version, std::move(change), {}, deadline, io.context.get_executor(),
                                      std::move(done));
  });
}

// A store that keeps as MemoryStore does, but holds each keep's completion until the test lets
// it go: a store whose disk is slow as the node stops.
class HeldKeepStore final : public agree::AcceptorStore {
 public:
  void load(agree::Slot slot, std::span<const agree::Name> names, bool every, bool values, std::function<void(agree::Held)> done) override {
    _store.load(slot, names, every, values, std::move(done));
  }
  // Holds only a keep that names the slot's served set, as founding's accept does: that keep's
  // completion publishes the slot advert, the path into the node. Every other keep completes.
  void keep(agree::Slot slot, const agree::Held& held, std::function<void(Kept)> done) override {
    Kept out;
    {
      std::lock_guard lock(_mutex);
      _store.keep(slot, held, [&out](Kept kept) { out = std::move(kept); });
      if (held.slot.served) {
        _kept = out;
        _held.push_back(std::move(done));
        return;
      }
    }
    done(std::move(out));
  }
  bool ready() const override { return true; }
  size_t waiting() {
    std::lock_guard lock(_mutex);
    return _held.size();
  }
  // Each held completion, posted as a store completes from its own thread; `ran` counts those
  // that have returned.
  size_t release(asio::io_context& io, std::atomic<size_t>& ran) {
    std::lock_guard lock(_mutex);
    const auto count = _held.size();
    for (auto& done : _held)
      asio::post(io, [done = std::move(done), kept = _kept, &ran] {
        done(kept);
        ++ran;
      });
    _held.clear();
    return count;
  }

 private:
  MemoryStore _store;
  Kept _kept;
  std::mutex _mutex;
  std::vector<std::function<void(Kept)>> _held;
};

// A store's completion that runs after its node is destroyed reaches nothing of the node (Mimir's
// ASan report on 0.9.2: a keep posted as the node stopped ran after it, and the slot advert it
// published wrote into the node's freed roster). Under ASan this is the use-after-free itself.
TEST(Node, AStoreCompletionAfterItsNodeIsDestroyedReachesNothingOfIt) {
  Pair pair({}, {}, true);
  auto store = std::make_shared<HeldKeepStore>();
  pair.a->context().agreement().serve("test", "test", store);
  // Not waited for: with the node gone, each slot's run waits out its own deadline in turn.
  pair.a->context().agreement().found("test", 2s, pair.io.context.get_executor(), [](agree::Outcome) {});
  ASSERT_TRUE(eventually([&] { return store->waiting() > 0; }));
  pair.b.reset();
  pair.a->stop();
  pair.a.reset();
  std::atomic<size_t> ran{0};
  const auto held = store->release(pair.io.context, ran);
  EXPECT_GT(held, 0u);
  // Each completion runs to its end, reaching nothing of the node it was for.
  EXPECT_TRUE(eventually([&] { return ran.load() == held; }));
}

// Agreement across the cluster (0.9.0): founded on A alone, the genesis, and grown to A and
// B by a transfer; a change on both nodes' acceptors commits on both, and the other node's
// strict read sees it; a node that does not advertise the space's capability is refused by
// the others, so it cannot make a majority.
TEST(Node, AgreementCommitsAcrossTheClusterAndRefusesANodeWithoutTheCapability) {
  Pair pair({}, {}, true);
  pair.b->start();
  const auto a = pair.a->context().identity().id();
  const auto b = pair.b->context().identity().id();
  ASSERT_TRUE(eventually([&] { return knows(*pair.a, b) && knows(*pair.b, a); }));
  pair.a->context().agreement().serve("test", "test", std::make_shared<MemoryStore>());
  pair.b->context().agreement().serve("test", "test", std::make_shared<MemoryStore>());
  pair.a->context().nodes().advertise("test", {});
  pair.b->context().nodes().advertise("test", {});
  const auto advertising = [](Node& node, const identity::NodeId& id) {
    for (const auto& record : node.context().nodes().active())
      if (record.id == id) return record.adverts.contains("test");
    return false;
  };
  ASSERT_TRUE(eventually([&] { return advertising(*pair.a, b) && advertising(*pair.b, a); }));

  const auto put = [](std::string value) {
    return [value](std::span<const std::optional<agree::Bytes>> current) {
      return agree::Changed{std::vector<std::optional<agree::Bytes>>(current.size(), agree::Bytes(value.begin(), value.end())), agree::Bytes{'o', 'k'}};
    };
  };
  const auto founded = told_within([&](auto done) { pair.a->context().agreement().found("test", 10s, pair.io.context.get_executor(), std::move(done)); });
  ASSERT_TRUE(founded.committed) << founded.why;
  // Before the transfer, a set naming B is refused: the slot is A's.
  const auto early = agree_now(*pair.a, {a, b}, 1, put("early"), 2s, pair.io);
  EXPECT_FALSE(early.committed);
  ASSERT_EQ(agree_now(*pair.a, {a}, 0, put("founded"), 10s, pair.io).committed, true);
  const auto grown = told_within(
      [&](auto done) { pair.a->context().agreement().transfer("test", k_slot, {a}, 0, {a, b}, 1, 10s, pair.io.context.get_executor(), std::move(done)); });
  ASSERT_TRUE(grown.committed) << grown.why;
  EXPECT_EQ(grown.names, std::vector<agree::Name>{agree::Name{'k'}});

  const auto written = agree_now(*pair.a, {a, b}, 1, put("agreed"), 10s, pair.io);
  ASSERT_TRUE(written.committed) << written.why;
  EXPECT_GE(written.accepted.size(), 2u);
  EXPECT_EQ(written.result, (agree::Bytes{'o', 'k'}));

  const auto read = told_within([&](auto done) {
    pair.b->context().agreement().read("test", k_slot, {agree::Name{'k'}}, {a, b}, 1, {}, 10s, pair.io.context.get_executor(), std::move(done));
  });
  ASSERT_TRUE(read.committed) << read.why;
  ASSERT_TRUE(read.values[0]);
  EXPECT_EQ(std::string(read.values[0]->begin(), read.values[0]->end()), "agreed");
  EXPECT_EQ(read.changed_at[0], written.changed_at[0]);

  // B no longer advertises the capability: A refuses it, and alone it is no majority.
  pair.b->context().nodes().withdraw("test");
  ASSERT_TRUE(eventually([&] { return !advertising(*pair.a, b); }));
  const auto refused = agree_now(*pair.b, {a, b}, 1, put("stranger"), 500ms, pair.io);
  EXPECT_FALSE(refused.committed);
  EXPECT_FALSE(refused.why.empty());
}

// A schedule the test runs by hand (NodeConfig::schedule): each task with the delay it was
// booked for, run only when the test says.
struct HandSchedule {
  std::mutex mutex;
  std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> tasks;
  std::function<void(std::chrono::milliseconds, std::function<void()>)> hook() {
    return [this](std::chrono::milliseconds after, std::function<void()> task) {
      std::lock_guard lock(mutex);
      tasks.emplace_back(after, std::move(task));
    };
  }
  // Runs every task booked so far, not those they book; how many ran.
  size_t run() {
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> now;
    {
      std::lock_guard lock(mutex);
      now.swap(tasks);
    }
    for (auto& [_, task] : now) task();
    return now.size();
  }
  bool booked(std::chrono::milliseconds after) {
    std::lock_guard lock(mutex);
    return std::any_of(tasks.begin(), tasks.end(), [after](const auto& task) { return task.first == after; });
  }
};

// The simulation's schedule (step 3 part 4, piece 2a): with one, the node starts no loop threads.
// Its loops' first steps run at start, and each books its next through the schedule, the
// membership pass a heartbeat after it ends; Time::after() books through it too, and fires
// only when the schedule runs it. After a stop, what was booked does nothing.
TEST(Node, WithAScheduleItsLoopsAndTimersRunWhenTheScheduleRunsThem) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  Node a(io.context, config(dir.path() / "a", key));
  a.start();
  HandSchedule schedule;
  auto b_config = config(dir.path() / "b", key, {{"127.0.0.1", a.port()}});
  b_config.schedule = schedule.hook();
  Node b(io.context, b_config);
  b.start();
  // The first membership pass ran at start: B learned A from its seed.
  ASSERT_TRUE(eventually([&] { return knows(b, a.context().identity().id()); }));
  ASSERT_TRUE(eventually([&] { return schedule.booked(b_config.heartbeat); }));
  // A timer through the context is the schedule's.
  std::atomic<bool> fired{false};
  b.context().time().after(1234ms, [&] { fired = true; });
  ASSERT_TRUE(schedule.booked(1234ms));
  EXPECT_FALSE(fired.load());
  EXPECT_GT(schedule.run(), 0u);
  EXPECT_TRUE(eventually([&] { return fired.load(); }));
  // A pass run again by the schedule exchanges again, and books its next.
  ASSERT_TRUE(eventually([&] { return schedule.booked(b_config.heartbeat); }));
  b.stop();
  // Booked before the stop, run after it: nothing happens and nothing more is booked.
  (void)schedule.run();
  EXPECT_FALSE(schedule.booked(b_config.heartbeat));
  a.stop();
}

// A stop waits for a scheduled step already running (0.10.1: hosted CI's TSan found a roster
// step writing while its node was destroyed, the step having passed its check before the stop).
// A step held running on an io thread keeps stop() from returning until it ends.
TEST(Node, AStopWaitsForAScheduledStepAlreadyRunning) {
  Io io;
  test::TempDir dir;
  const auto key = cluster_key(dir.path());
  HandSchedule schedule;
  auto b_config = config(dir.path() / "b", key);
  b_config.schedule = schedule.hook();
  Node b(io.context, b_config);
  b.start();
  std::promise<void> running;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<bool> ended{false};
  b.context().time().after(4321ms, [&running, released, &ended] {
    running.set_value();
    released.wait();
    ended = true;
  });
  ASSERT_TRUE(schedule.booked(4321ms));
  (void)schedule.run();
  ASSERT_EQ(running.get_future().wait_for(10s), std::future_status::ready);
  auto stopped = std::async(std::launch::async, [&b] { b.stop(); });
  // While the step runs, the stop waits for it. The bound only gives a wrong stop the time to
  // return; a right one never does while the step is held.
  EXPECT_EQ(stopped.wait_for(200ms), std::future_status::timeout) << "stop returned while a scheduled step was still running";
  release.set_value();
  ASSERT_EQ(stopped.wait_for(10s), std::future_status::ready);
  EXPECT_TRUE(ended.load()) << "stop returned before the step it waited for had ended";
}

}  // namespace
}  // namespace dannan
