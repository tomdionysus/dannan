//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Level 0 (wire v1, sections 5 and 6): the payloads, the classification of
// a node's own reachability from where peers saw it, the observation from a
// fixed local port, the TCP simultaneous open, and two nodes that take no
// inbound connection meeting through a broker (punched) or a relay
// (relayed), each pair then calling on both lanes. Over loopback, where
// every mapping is preserved; the compose bed proves the same through real
// NATs.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <fstream>
#include <future>
#include <thread>

#include "dannan/node.h"
#include "dannan/reach/paths.h"
#include "dannan/test/fixtures/ports.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::reach {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using membership::PathClass;

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

identity::NodeId id(uint8_t n) {
  identity::NodeId out;
  out.bytes[0] = n;
  return out;
}

TEST(LevelZero, ThePayloadsRoundTrip) {
  const net::PathToken token{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const auto request = decode_punch_request(encode(PunchRequest{id(2), net::Lane::data, {"198.51.100.1", 4000}}));
  EXPECT_EQ(request.target, id(2));
  EXPECT_EQ(request.lane, net::Lane::data);
  EXPECT_EQ(request.from, (net::Endpoint{"198.51.100.1", 4000}));
  const auto go = decode_punch_go(encode(PunchGo{id(3), net::Lane::control, token, {"h", 5}}));
  EXPECT_EQ(go.peer, id(3));
  EXPECT_EQ(go.token, token);
  EXPECT_EQ(go.at, (net::Endpoint{"h", 5}));
  const auto punch = decode_punch_reply(encode(PunchReply{true, "why", {"h", 6}, token}));
  EXPECT_TRUE(punch.accepted);
  EXPECT_EQ(punch.why, "why");
  EXPECT_EQ(punch.at.port, 6);
  EXPECT_EQ(punch.token, token);
  EXPECT_EQ(decode_relay_open(encode(RelayOpen{id(4), net::Lane::data})).target, id(4));
  const auto opened = decode_relay_open_reply(encode(RelayOpenReply{77, "", token}));
  EXPECT_EQ(opened.circuit, 77u);
  EXPECT_EQ(opened.token, token);
  const auto offer = decode_relay_offer(encode(RelayOffer{78, id(5), net::Lane::control, token}));
  EXPECT_EQ(offer.circuit, 78u);
  EXPECT_EQ(offer.from, id(5));
  EXPECT_FALSE(decode_relay_offer_reply(encode(RelayOfferReply{false, "full"})).accepted);
  EXPECT_EQ(decode_relay_data(encode(RelayData{79, 5, {9, 8, 7}})).bytes, (net::Bytes{9, 8, 7}));
  EXPECT_EQ(decode_relay_data(encode(RelayData{79, 5, {9, 8, 7}})).sequence, 5u);
  EXPECT_EQ(decode_relay_close(encode(RelayClose{80, "done", 12})).why, "done");
  EXPECT_EQ(decode_relay_close(encode(RelayClose{80, "done", 12})).chunks, 12u);
  EXPECT_EQ(decode_relay_close(encode(RelayClose{80, "lost"})).chunks, unknown_chunks);
  // A path is for CONTROL or DATA; trailing bytes are refused.
  net::Writer probe_lane;
  probe_lane.fixed(id(2).bytes);
  probe_lane.u8(static_cast<uint8_t>(net::Lane::probe));
  EXPECT_THROW(decode_relay_open(probe_lane.data()), net::DecodeError);
  auto trailing = encode(RelayClose{1, "x"});
  trailing.push_back(0);
  EXPECT_THROW(decode_relay_close(trailing), net::DecodeError);
}

// Two peers that saw one local port at one external port: an endpoint-
// independent mapping, punchable. Two ports: relay only. One peer: the
// port must have been preserved.
TEST(LevelZero, ANodeIsClassifiedByWhatPeersSaw) {
  EXPECT_EQ(classify({4000, {{"203.0.113.1", 51000}, {"203.0.113.1", 51000}}}), PathClass::punchable);
  EXPECT_EQ(classify({4000, {{"203.0.113.1", 51000}, {"203.0.113.1", 51001}}}), PathClass::relay_only);
  EXPECT_EQ(classify({4000, {{"203.0.113.1", 4000}}}), PathClass::punchable);
  EXPECT_EQ(classify({4000, {{"203.0.113.1", 4001}}}), PathClass::relay_only);
  EXPECT_EQ(classify({4000, {}}), PathClass::relay_only);
}

NodeConfig config(const std::filesystem::path& state, const identity::ClusterKey& key, std::vector<net::Endpoint> bootstrap, Inbound inbound) {
  NodeConfig out;
  out.state_path = state;
  out.key = key;
  out.listen_host = "127.0.0.1";
  out.advertise_host = "127.0.0.1";
  out.port = 0;
  out.bootstrap = std::move(bootstrap);
  out.inbound = inbound;
  out.heartbeat = 100ms;
  out.dead_after = 3s;
  out.telemetry_interval = 200ms;
  out.connect_timeout = 2s;
  out.reprobe_while_incapable = 1h;
  out.version = "level-0-test";
  return out;
}

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds bound = 15s) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return condition();
}

// Seen from a fixed local port, through the probe lane: the address the
// peer's socket saw, and the port it left from.
TEST(LevelZero, ANodeIsObservedFromAFixedLocalPort) {
  Io io;
  test::TempDir dir;
  std::ofstream(dir.path() / "k") << "observe";
  const auto key = identity::load_cluster_key(dir.path() / "k");
  Node peer(io.context, config(dir.path() / "peer", key, {}, Inbound::yes));
  Node other(io.context, config(dir.path() / "other", key, {}, Inbound::yes));
  peer.start();
  other.start();
  membership::NodeRecord self;
  self.id = identity::NodeId::random();
  self.host = "127.0.0.1";
  // What is observed, not how fast: a loaded, instrumented process (the
  // full TSan suite) can take seconds over a loopback handshake.
  const auto port = test::free_port();
  const auto [bound, seen] = observe_from(io.context, key, self, port, {"127.0.0.1", peer.port()}, peer.context().identity().id(), 10s);
  EXPECT_EQ(bound, port);
  EXPECT_EQ(seen, (net::Endpoint{"127.0.0.1", port}));
  // From the same port to another peer: several connections leave from one
  // port, which is what tells an endpoint-independent mapping.
  EXPECT_EQ(observe_from(io.context, key, self, port, {"127.0.0.1", other.port()}, other.context().identity().id(), 10s).second.port, port);
  EXPECT_THROW(observe_from(io.context, key, self, 0, {"127.0.0.1", peer.port()}, id(9), 10s), std::runtime_error);
  other.stop();
  peer.stop();
}

// Both ends connect to each other from their punch ports at once: one
// connection, each end holding a connected descriptor.
TEST(LevelZero, ASimultaneousOpenConnectsBothEnds) {
  Io io;
  const auto a = test::free_port();
  const auto b = test::free_port();
  std::promise<std::pair<int, std::string>> at_a, at_b;
  simultaneous_open(io.context, a, {"127.0.0.1", b}, 3s, [&](int fd, int, std::string why) { at_a.set_value({fd, why}); });
  simultaneous_open(io.context, b, {"127.0.0.1", a}, 3s, [&](int fd, int, std::string why) { at_b.set_value({fd, why}); });
  const auto [fa, why_a] = at_a.get_future().get();
  const auto [fb, why_b] = at_b.get_future().get();
  ASSERT_GE(fa, 0) << why_a;
  ASSERT_GE(fb, 0) << why_b;
  for (const int fd : {fa, fb}) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
  const char hello[] = "hi";
  ASSERT_EQ(::send(fa, hello, 2, 0), 2);
  char got[2] = {};
  ASSERT_EQ(::recv(fb, got, 2, MSG_WAITALL), 2);
  EXPECT_EQ(std::string(got, 2), "hi");
  ::close(fa);
  ::close(fb);
}

// The ends' clocks and schedulers never agree exactly. Where nothing drops a SYN
// (loopback, a LAN) the first to connect is refused at once, and if both then retry
// in step they stay out of step: the open must still meet within the window.
TEST(LevelZero, ASimultaneousOpenMeetsWhenTheEndsBeginOutOfStep) {
  Io io;
  const auto a = test::free_port();
  const auto b = test::free_port();
  std::promise<std::pair<int, std::string>> at_a, at_b;
  simultaneous_open(io.context, a, {"127.0.0.1", b}, 3s, [&](int fd, int, std::string why) { at_a.set_value({fd, why}); });
  asio::steady_timer late(io.context, std::chrono::microseconds(2500));
  late.async_wait([&](const boost::system::error_code&) {
    simultaneous_open(io.context, b, {"127.0.0.1", a}, 3s, [&](int fd, int, std::string why) { at_b.set_value({fd, why}); });
  });
  const auto [fa, why_a] = at_a.get_future().get();
  const auto [fb, why_b] = at_b.get_future().get();
  EXPECT_GE(fa, 0) << why_a;
  EXPECT_GE(fb, 0) << why_b;
  for (const int fd : {fa, fb})
    if (fd >= 0) ::close(fd);
}

TEST(LevelZero, ASimultaneousOpenToNobodyGivesUp) {
  Io io;
  std::promise<std::pair<int, std::string>> done;
  simultaneous_open(io.context, test::free_port(), {"127.0.0.1", test::free_port()}, 500ms, [&](int fd, int, std::string why) { done.set_value({fd, why}); });
  const auto [fd, why] = done.get_future().get();
  EXPECT_LT(fd, 0);
  EXPECT_NE(why.find("did not complete"), std::string::npos) << why;
}

constexpr net::MessageType echo_control{1024};
constexpr net::MessageType echo_data{1025};

net::Handled answer(const membership::NodeRecord&, sched::FrameType, const net::Message& request) {
  return net::Handled::processed({net::message::ok, request.payload, {}});
}

// A hub that takes inbound connections (and relays, if asked), and two
// nodes that take none, both bootstrapped from it.
struct Cluster {
  Io io;
  test::TempDir dir;
  identity::ClusterKey key;
  std::unique_ptr<Node> hub, a, b;
  bool forced_a, forced_b;

  Cluster(bool relay, std::optional<PathClass> a_class, std::optional<PathClass> b_class, std::chrono::milliseconds upgrade_every = std::chrono::minutes(1),
          size_t relay_data_circuits = 32, std::chrono::milliseconds dead_after = 3s, const std::function<void(NodeConfig&)>& tune = {})
      : forced_a(a_class.has_value()), forced_b(b_class.has_value()) {
    std::ofstream(dir.path() / "k") << "level zero";
    key = identity::load_cluster_key(dir.path() / "k");
    auto hub_config = config(dir.path() / "hub", key, {}, Inbound::yes);
    hub_config.relay = relay;
    hub_config.relay_data_circuits = relay_data_circuits;
    hub_config.dead_after = dead_after;
    if (tune) tune(hub_config);
    hub = std::make_unique<Node>(io.context, hub_config);
    hub->start();
    const std::vector<net::Endpoint> seed{{"127.0.0.1", hub->port()}};
    auto a_config = config(dir.path() / "a", key, seed, Inbound::no);
    a_config.path_class = a_class;
    a_config.path_upgrade_every = upgrade_every;
    a_config.dead_after = dead_after;
    auto b_config = config(dir.path() / "b", key, seed, Inbound::no);
    b_config.path_class = b_class;
    b_config.path_upgrade_every = upgrade_every;
    b_config.dead_after = dead_after;
    if (tune) {
      tune(a_config);
      tune(b_config);
    }
    a = std::make_unique<Node>(io.context, a_config);
    b = std::make_unique<Node>(io.context, b_config);
    for (auto* node : {a.get(), b.get()}) {
      node->context().routes().add(echo_control, {"echo_control", net::Lanes::control, net::Frames::any, false}, answer);
      node->context().routes().add(echo_data, {"echo_data", net::Lanes::data, net::Frames::not_control, false}, answer);
      node->start();
    }
  }
  ~Cluster() {
    for (auto* node : {b.get(), a.get(), hub.get()})
      if (node) node->stop();
  }

  std::optional<membership::NodeRecord> record(Node& of, Node& in) {
    for (const auto& node : in.context().nodes().active())
      if (node.id == of.context().identity().id()) return node;
    return std::nullopt;
  }
  // Each knows the other, and both have learnt their path class (a forced
  // one is known from the start).
  bool ready() {
    const auto ab = record(*b, *a);
    const auto ba = record(*a, *b);
    const auto learnt = [](Node& node, bool forced) { return forced || !node.context().nodes().self().candidates.empty(); };
    return ab && ba && learnt(*a, forced_a) && learnt(*b, forced_b);
  }
  net::CallOutcome echo(Node& from, Node& to, bool data) {
    const auto peer = record(to, from);
    if (!peer) return {std::nullopt, "not known", {}};
    return from.context().routes().call_and_wait(*peer, data ? echo_data : echo_control, net::Bytes{4, 2},
                                                 data ? sched::FrameType::foreground : sched::FrameType::control, 10s);
  }
};

// Neither takes inbound connections; both are punchable: the hub brokers a
// simultaneous open, the handshake proves the token, and the pair calls
// each other on both lanes over punched sessions.
TEST(LevelZero, TwoNodesBehindNatMeetByAPunch) {
  Cluster cluster(false, std::nullopt, std::nullopt);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  EXPECT_EQ(cluster.a->context().nodes().self().path_class, PathClass::punchable);
  for (const bool data : {false, true}) {
    const auto outcome = cluster.echo(*cluster.a, *cluster.b, data);
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->payload, (net::Bytes{4, 2}));
  }
  const auto b = cluster.b->context().identity().id();
  EXPECT_EQ(cluster.a->context().nodes().path(b, net::Lane::control), net::Purpose::punched);
  EXPECT_EQ(cluster.a->context().nodes().path(b, net::Lane::data), net::Purpose::punched);
  // The other way rides the same sessions.
  const auto back = cluster.echo(*cluster.b, *cluster.a, false);
  ASSERT_TRUE(back.reply) << back.failure;
  EXPECT_GE(cluster.hub->diagnostics().paths.punches_brokered, 2u);
  EXPECT_GE(cluster.a->diagnostics().paths.punches_made, 2u);
}

// One end's NAT cannot be punched: the pair meets through the relay, which
// carries only ciphertext and counts the bytes per circuit.
TEST(LevelZero, TwoNodesMeetThroughARelayWhenAPunchCannotWork) {
  Cluster cluster(true, PathClass::relay_only, std::nullopt);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  EXPECT_EQ(cluster.a->context().nodes().self().path_class, PathClass::relay_only);
  for (const bool data : {false, true}) {
    const auto outcome = cluster.echo(*cluster.a, *cluster.b, data);
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->payload, (net::Bytes{4, 2}));
  }
  const auto b = cluster.b->context().identity().id();
  EXPECT_EQ(cluster.a->context().nodes().path(b, net::Lane::control), net::Purpose::relayed);
  EXPECT_EQ(cluster.a->context().nodes().path(b, net::Lane::data), net::Purpose::relayed);
  const auto back = cluster.echo(*cluster.b, *cluster.a, true);
  ASSERT_TRUE(back.reply) << back.failure;
  const auto relay = cluster.hub->diagnostics().paths;
  EXPECT_EQ(relay.circuits_relayed, 2u);
  ASSERT_EQ(relay.circuits.size(), 2u);
  EXPECT_GT(relay.relayed_bytes, 0u);
  for (const auto& circuit : relay.circuits) EXPECT_GT(circuit.bytes, 0u);
}

// A transfer much larger than anything queued on the way: bytes through a relay are paced
// end to end, so nothing is dropped and the circuit lives. Macha's size.
TEST(LevelZero, ALargeTransferCrossesARelayIntact) {
  // A bulk case, on an instrumented build as well: a budget that tests the bytes, not the speed.
  Cluster cluster(true, PathClass::relay_only, PathClass::relay_only, std::chrono::minutes(1), 32, 30s);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto peer = cluster.record(*cluster.b, *cluster.a);
  ASSERT_TRUE(peer.has_value());
  net::Bytes payload(32 * 1024 * 1024);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 31 >> 3);
  const auto outcome = cluster.a->context().routes().call_and_wait(*peer, echo_data, payload, sched::FrameType::foreground, 120s);
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(outcome.reply->payload, payload);
  EXPECT_EQ(cluster.a->context().nodes().path(peer->id, net::Lane::data), net::Purpose::relayed);
  // Bulk on DATA at each hop, as wire v1 section 6 has it: never in front of
  // the pings on the CONTROL session to the relay.
  bool data_circuit = false;
  for (const auto& circuit : cluster.hub->diagnostics().paths.circuits) {
    if (circuit.lane != net::Lane::data) continue;
    data_circuit = true;
    EXPECT_EQ(circuit.carrier_a, net::Lane::data);
    EXPECT_EQ(circuit.carrier_b, net::Lane::data);
  }
  EXPECT_TRUE(data_circuit);
  // Twice: the circuit is still good.
  EXPECT_TRUE(cluster.a->context().routes().call_and_wait(*peer, echo_data, net::Bytes(1024, 3), sched::FrameType::foreground, 60s).reply);
}

// Both ways at once, on one circuit, with the control circuit answering all
// through it: credit for one direction never waits on the other's bytes, and
// control never queues behind data (law 1).
TEST(LevelZero, TransfersBothWaysShareARelayAndControlStillAnswers) {
  // A bulk case, on an instrumented build as well: a budget that tests the bytes, not the speed.
  Cluster cluster(true, PathClass::relay_only, PathClass::relay_only, std::chrono::minutes(1), 32, 30s);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto to_b = cluster.record(*cluster.b, *cluster.a);
  const auto to_a = cluster.record(*cluster.a, *cluster.b);
  ASSERT_TRUE(to_b && to_a);
  net::Bytes payload(16 * 1024 * 1024);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 17 >> 2);
  const auto send = [&](Node& from, const membership::NodeRecord& peer) {
    return std::async(std::launch::async,
                      [&from, peer, &payload] { return from.context().routes().call_and_wait(peer, echo_data, payload, sched::FrameType::foreground, 120s); });
  };
  auto forward = send(*cluster.a, *to_b);
  auto backward = send(*cluster.b, *to_a);
  for (int i = 0; i < 20; ++i) {
    const auto control = cluster.echo(*cluster.a, *cluster.b, false);
    ASSERT_TRUE(control.reply) << control.failure;
  }
  const auto there = forward.get();
  const auto back = backward.get();
  ASSERT_TRUE(there.reply) << there.failure;
  ASSERT_TRUE(back.reply) << back.failure;
  EXPECT_EQ(there.reply->payload, payload);
  EXPECT_EQ(back.reply->payload, payload);
}

// Many transfers at once, far past what any one queue on the way would hold
// if a circuit were not paced: nothing is refused and every byte arrives.
TEST(LevelZero, ManyTransfersAtOnceCrossARelayIntact) {
  // A bulk case, on an instrumented build as well: a budget that tests the bytes, not the speed.
  Cluster cluster(true, PathClass::relay_only, PathClass::relay_only, std::chrono::minutes(1), 32, 30s);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto peer = cluster.record(*cluster.b, *cluster.a);
  ASSERT_TRUE(peer.has_value());
  std::vector<std::future<std::pair<net::Bytes, net::CallOutcome>>> running;
  for (int t = 0; t < 8; ++t) {
    running.push_back(std::async(std::launch::async, [&cluster, peer, t] {
      net::Bytes payload(4 * 1024 * 1024, static_cast<uint8_t>(t + 1));
      auto outcome = cluster.a->context().routes().call_and_wait(*peer, echo_data, payload, sched::FrameType::foreground, 120s);
      return std::make_pair(std::move(payload), std::move(outcome));
    }));
  }
  for (auto& one : running) {
    const auto [payload, outcome] = one.get();
    ASSERT_TRUE(outcome.reply) << outcome.failure;
    EXPECT_EQ(outcome.reply->payload, payload);
  }
  EXPECT_EQ(cluster.a->context().nodes().path(peer->id, net::Lane::data), net::Purpose::relayed);
}

// A relayed pair whose relay-only end becomes punchable moves up: the lower
// id punches, both ends keep the punched session over the relayed one, and
// the relayed one retires. Calls keep working throughout.
TEST(LevelZero, ARelayedPairMovesUpToAPunch) {
  Cluster cluster(true, PathClass::relay_only, PathClass::relay_only, 200ms);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  ASSERT_TRUE(cluster.echo(*cluster.a, *cluster.b, false).reply);
  const auto a = cluster.a->context().identity().id();
  const auto b = cluster.b->context().identity().id();
  ASSERT_EQ(cluster.a->context().nodes().path(b, net::Lane::control), net::Purpose::relayed);
  // Both ends' NATs turn out punchable.
  cluster.a->membership().set_path_class(PathClass::punchable);
  cluster.b->membership().set_path_class(PathClass::punchable);
  EXPECT_TRUE(eventually([&] {
    return cluster.a->context().nodes().path(b, net::Lane::control) == net::Purpose::punched &&
           cluster.b->context().nodes().path(a, net::Lane::control) == net::Purpose::punched;
  }));
  const auto lower = a < b ? cluster.a.get() : cluster.b.get();
  // Counted once the upgrade's call returns, which may be after the path shows.
  EXPECT_TRUE(eventually([&] { return lower->diagnostics().paths.upgrades_made >= 1u; }));
  const auto outcome = cluster.echo(*cluster.a, *cluster.b, false);
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  // The relay's circuit goes with the relayed session.
  EXPECT_TRUE(eventually([&] { return cluster.hub->diagnostics().paths.circuits.empty(); }));
}

// The relay's DATA budget: past it a DATA circuit is refused, while CONTROL
// circuits are unconditional.
TEST(LevelZero, ARelayRefusesDataPastItsBudgetButNeverControl) {
  Cluster cluster(true, PathClass::relay_only, std::nullopt, std::chrono::minutes(1), 0);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto control = cluster.echo(*cluster.a, *cluster.b, false);
  ASSERT_TRUE(control.reply) << control.failure;
  const auto data = cluster.echo(*cluster.a, *cluster.b, true);
  ASSERT_FALSE(data.reply);
  EXPECT_NE(data.failure.find("DATA budget"), std::string::npos) << data.failure;
  EXPECT_GE(cluster.hub->diagnostics().paths.data_circuits_refused, 1u);
}

// No punch (one end relay only) and no relay: the call fails, saying why.
TEST(LevelZero, WithoutABrokerOrARelayThereIsNoPath) {
  Cluster cluster(false, PathClass::relay_only, std::nullopt);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto outcome = cluster.echo(*cluster.a, *cluster.b, false);
  ASSERT_FALSE(outcome.reply);
  EXPECT_NE(outcome.failure.find("no path"), std::string::npos) << outcome.failure;
  EXPECT_NE(outcome.failure.find("relay"), std::string::npos) << outcome.failure;
}

namespace {
size_t data_circuits(Node& relay) {
  size_t out = 0;
  for (const auto& circuit : relay.diagnostics().paths.circuits)
    if (circuit.lane == net::Lane::data) ++out;
  return out;
}
}  // namespace

// A chunk the sender's session took and never wrote dies with that session: the count
// on the close cannot be honoured, and nothing waits for it. The relay lets the circuit
// go and the far end closes, within the bound (Macha's review of 7e35338).
TEST(LevelZero, AChunkLostWithTheSendersSessionEndsTheCircuitWithinTheBound) {
  Cluster cluster(true, PathClass::relay_only, PathClass::relay_only);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto peer = cluster.record(*cluster.b, *cluster.a);
  ASSERT_TRUE(peer.has_value());
  const auto ends_before = cluster.b->diagnostics().paths.ends;
  auto transfer = std::async(std::launch::async, [&] {
    return cluster.a->context().routes().call_and_wait(*peer, echo_data, net::Bytes(32 * 1024 * 1024, 7), sched::FrameType::foreground, 60s);
  });
  ASSERT_TRUE(eventually([&] { return data_circuits(*cluster.hub) > 0 && cluster.hub->diagnostics().paths.relayed_bytes > 4 * 1024 * 1024; }));
  cluster.a->close_route(cluster.hub->context().identity().id(), net::Lane::data, "severed by the test");
  (void)transfer.get();
  EXPECT_TRUE(eventually([&] { return data_circuits(*cluster.hub) == 0; }, 20s));
  EXPECT_TRUE(eventually([&] { return cluster.b->diagnostics().paths.ends <= ends_before; }, 20s));
  // The way (a) is for: A's failed send closed with its count unknown.
  EXPECT_GT(cluster.a->diagnostics().paths.closes_unknown, 0u);
}

// The relay's chunks to the far end and its close go by different sessions, and the one
// with chunks dies: the far end stops waiting for them within the bound and leaves.
TEST(LevelZero, AChunkLostOnTheWayToTheFarEndEndsItWithinTheBound) {
  Cluster cluster(true, PathClass::relay_only, PathClass::relay_only);
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto peer = cluster.record(*cluster.b, *cluster.a);
  ASSERT_TRUE(peer.has_value());
  const auto ends_before = cluster.b->diagnostics().paths.ends;
  auto transfer = std::async(std::launch::async, [&] {
    return cluster.a->context().routes().call_and_wait(*peer, echo_data, net::Bytes(32 * 1024 * 1024, 7), sched::FrameType::foreground, 60s);
  });
  ASSERT_TRUE(eventually([&] { return data_circuits(*cluster.hub) > 0 && cluster.hub->diagnostics().paths.relayed_bytes > 4 * 1024 * 1024; }));
  cluster.hub->close_route(cluster.b->context().identity().id(), net::Lane::data, "severed by the test");
  (void)transfer.get();
  EXPECT_TRUE(eventually([&] { return data_circuits(*cluster.hub) == 0; }, 20s));
  EXPECT_TRUE(eventually([&] { return cluster.b->diagnostics().paths.ends <= ends_before; }, 20s));
  // The way this one goes: the relay lost B's session and closed the circuit itself.
  EXPECT_GT(cluster.hub->diagnostics().paths.relay_ends_lost, 0u);
}

namespace {
// A circuit A to B over the relay with one chunk lost there, `forwarding` or on receipt:
// the first the relay sees once the test arms it, after the session is up. A's end then
// closes in order, its count exact, and the bounds are what end it.
std::string counters(Cluster& cluster) {
  std::string out;
  for (auto* node : {cluster.a.get(), cluster.hub.get(), cluster.b.get()}) {
    const auto paths = node->diagnostics().paths;
    out += "unknown=" + std::to_string(paths.closes_unknown) + " expired=" + std::to_string(paths.relay_closes_expired) +
           " finishes=" + std::to_string(paths.finishes_expired) + " ends=" + std::to_string(paths.ends) +
           " ends_lost=" + std::to_string(paths.relay_ends_lost) + " relays_made=" + std::to_string(paths.relays_made) +
           " punches_made=" + std::to_string(paths.punches_made) + " upgrades=" + std::to_string(paths.upgrades_made) +
           " relayed=" + std::to_string(paths.relayed_bytes) + " circuits=" + std::to_string(paths.circuits.size()) + "; ";
  }
  return out;
}

struct LostChunk {
  std::shared_ptr<std::atomic<bool>> armed = std::make_shared<std::atomic<bool>>(false);
  Cluster cluster;
  explicit LostChunk(bool forwarding)
      : cluster(true, PathClass::relay_only, PathClass::relay_only, std::chrono::minutes(1), 32, 3s, [this, forwarding](NodeConfig& c) {
          c.relay_finish_wait = 300ms;
          if (c.relay) c.relay_drop_hook = [armed = armed, forwarding](uint64_t, bool at) { return at == forwarding && armed->exchange(false); };
        }) {}
};
}  // namespace

// The relay holds a counted close whose chunk never came, and after the bound lets it go
// as unknown: the far end closes and the circuit is gone.
TEST(LevelZero, ARelayLetsACountedCloseGoWhenItsChunksNeverCome) {
  LostChunk lost(false);
  auto& cluster = lost.cluster;
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto peer = cluster.record(*cluster.b, *cluster.a);
  ASSERT_TRUE(peer.has_value());
  const auto ends_before = cluster.b->diagnostics().paths.ends;
  auto transfer = std::async(std::launch::async, [&] {
    return cluster.a->context().routes().call_and_wait(*peer, echo_data, net::Bytes(8 * 1024 * 1024, 7), sched::FrameType::foreground, 30s);
  });
  ASSERT_TRUE(eventually([&] { return data_circuits(*cluster.hub) > 0 && cluster.hub->diagnostics().paths.relayed_bytes > 256 * 1024; }));
  *lost.armed = true;
  ASSERT_TRUE(eventually([&] { return !lost.armed->load(); }));
  // A closes its relayed session: its end drains in order and sends an exact count.
  cluster.a->close_route(cluster.b->context().identity().id(), net::Lane::data, "closed by the test");
  const auto ended = transfer.get();
  EXPECT_TRUE(eventually([&] { return cluster.hub->diagnostics().paths.relay_closes_expired == 1; }, 10s)) << counters(cluster) << ended.failure;
  EXPECT_TRUE(eventually([&] { return data_circuits(*cluster.hub) == 0; }, 10s));
  EXPECT_TRUE(eventually([&] { return cluster.b->diagnostics().paths.ends <= ends_before; }, 10s));
}

// The relay forwards a counted close but a chunk toward the far end is lost: the far end
// stops waiting for it after the bound, and leaves.
TEST(LevelZero, AFarEndStopsWaitingForAChunkThatNeverComes) {
  LostChunk lost(true);
  auto& cluster = lost.cluster;
  ASSERT_TRUE(eventually([&] { return cluster.ready(); }));
  const auto peer = cluster.record(*cluster.b, *cluster.a);
  ASSERT_TRUE(peer.has_value());
  const auto ends_before = cluster.b->diagnostics().paths.ends;
  auto transfer = std::async(std::launch::async, [&] {
    return cluster.a->context().routes().call_and_wait(*peer, echo_data, net::Bytes(8 * 1024 * 1024, 7), sched::FrameType::foreground, 30s);
  });
  ASSERT_TRUE(eventually([&] { return data_circuits(*cluster.hub) > 0 && cluster.hub->diagnostics().paths.relayed_bytes > 256 * 1024; }));
  *lost.armed = true;
  ASSERT_TRUE(eventually([&] { return !lost.armed->load(); }));
  cluster.a->close_route(cluster.b->context().identity().id(), net::Lane::data, "closed by the test");
  const auto ended = transfer.get();
  EXPECT_TRUE(eventually([&] { return cluster.b->diagnostics().paths.finishes_expired == 1; }, 10s)) << counters(cluster) << ended.failure;
  EXPECT_TRUE(eventually([&] { return cluster.b->diagnostics().paths.ends <= ends_before; }, 10s));
}

}  // namespace
}  // namespace dannan::reach
