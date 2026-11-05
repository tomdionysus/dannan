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
#include "dannan/test/fixtures/recording_sink.h"
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
  // Membership moved when A joined, and an advert does not move it.
  const auto members = pair.b->context().events().count(status::NodeEvents::membership);
  EXPECT_GT(members, 0u);
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

}  // namespace
}  // namespace dannan
