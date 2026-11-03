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
#include <fstream>
#include <future>
#include <thread>

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
    const auto all = pair.a->telemetry().all();
    return std::any_of(all.begin(), all.end(), [&](const status::Telemetry& t) { return t.node_id() == b && t.text(status::field::version) == "0.1.0-test"; });
  }));
  EXPECT_TRUE(eventually([&] { return pair.b->context().nodes().latency(a).has_value(); }));
}

// A hosted system that is nothing but a counter: a user type registered
// through the Context, called across the cluster, its count in Status.
TEST(Node, ACounterRunsOnIt) {
  constexpr net::MessageType add{300};
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
  EXPECT_THROW(pair.a->context().routes().add(net::MessageType{302}, {"late", net::Lanes::control, net::Frames::any, false}, {}), std::logic_error);
}

// A call never waits past its deadline.
TEST(Node, ACallEndsAtItsDeadline) {
  constexpr net::MessageType slow{301};
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
  EXPECT_TRUE(eventually([&] { return pair.b->telemetry().local() && pair.b->telemetry().local()->phase() == status::Phase::recovering; }));
  pair.b->readiness().ready("metadata");
  EXPECT_EQ(pair.b->readiness().phase(), status::Phase::ready);
  EXPECT_TRUE(eventually([&] { return pair.b->telemetry().local()->phase() == status::Phase::ready; }));
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
  pair.a->propagate_identity_reset(reset);
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

TEST(Node, ReportsItsContextVersion) {
  Pair pair;
  EXPECT_EQ(pair.a->context().version(), context_version);
}

}  // namespace
}  // namespace dannan
