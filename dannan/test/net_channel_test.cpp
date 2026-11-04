//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The authenticated channel, sans I/O: the inventory's transport rows "frame
// size negotiated at the handshake, bounded by config", "authenticated
// encrypted channel: AEAD per fragment with counters, a session id", and
// wire v1's handshake (section 2): a version range from 1, the highest
// common version chosen and none refused, the observed address in the ack,
// and the path token of a punched or relayed connection inside the hello.

#include <gtest/gtest.h>

#include <fstream>
#include <tuple>

#include "dannan/net/channel.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::net {
namespace {

using sched::FrameType;

identity::ClusterKey key(const std::string& secret) {
  test::TempDir dir;
  std::ofstream(dir.path() / "k") << secret;
  return identity::load_cluster_key(dir.path() / "k");
}

membership::NodeRecord node(const std::string& host) {
  membership::NodeRecord record;
  record.id = identity::NodeId::random();
  record.host = host;
  record.port = 7437;
  record.failure_domain = "test";
  return record;
}

// Strips the u32 length a blob travels with.
std::span<const uint8_t> body(const Bytes& blob) { return std::span(blob).subspan(4); }

struct Pair {
  Established client;
  Established server;
};

Pair handshake(const identity::ClusterKey& client_key, size_t client_frame, const identity::ClusterKey& server_key, size_t server_frame,
               Lane lane = Lane::control, const std::string& client_host = "10.0.0.1") {
  const auto a = node(client_host);
  const auto b = node("10.0.0.2");
  ClientHandshake client(client_key, a, client_frame, lane);
  auto reply = server_handshake(server_key, b, server_frame, body(client.hello_blob()), {"192.0.2.7", 50123});
  auto established = client.finish(body(reply.response_blob));
  return {std::move(established), std::move(reply.session)};
}

TEST(Channel, BothEndsAgreeOnPeersLaneAndSessionId) {
  const auto k = key("cluster");
  const auto pair = handshake(k, 65536, k, 65536, Lane::data);
  EXPECT_EQ(pair.client.lane, Lane::data);
  EXPECT_EQ(pair.server.lane, Lane::data);
  EXPECT_EQ(pair.client.keys.session_id, pair.server.keys.session_id);
  EXPECT_EQ(pair.client.keys.tx, pair.server.keys.rx);
  EXPECT_EQ(pair.client.keys.rx, pair.server.keys.tx);
  EXPECT_NE(pair.client.keys.tx, pair.client.keys.rx);
  EXPECT_EQ(pair.client.peer.host, "10.0.0.2");
  EXPECT_EQ(pair.server.peer.host, "10.0.0.1");
}

TEST(Channel, NoTwoSessionsShareAnIdOrKeys) {
  const auto k = key("cluster");
  const auto a = handshake(k, 65536, k, 65536);
  const auto b = handshake(k, 65536, k, 65536);
  EXPECT_NE(a.client.keys.session_id, b.client.keys.session_id);
  EXPECT_NE(a.client.keys.tx, b.client.keys.tx);
}

// A peer that advertises no address is known by the address it came from.
TEST(Channel, AWildcardOrEmptyHostIsTheRemoteAddress) {
  const auto k = key("cluster");
  for (const auto* host : {"", "0.0.0.0", "::"}) EXPECT_EQ(handshake(k, 65536, k, 65536, Lane::control, host).server.peer.host, "192.0.2.7") << host;
}

TEST(Channel, BothEndsAgreeOnTheSmallerFrameLimit) {
  const auto k = key("cluster");
  for (const auto& [c, s] : {std::pair<size_t, size_t>{4096, 65536}, {65536, 4096}}) {
    const auto pair = handshake(k, c, k, s);
    EXPECT_EQ(pair.client.max_frame, 4096u);
    EXPECT_EQ(pair.server.max_frame, 4096u);
  }
}

TEST(Channel, AFrameLimitOutsideFourKToFourMIsRefused) {
  const auto k = key("cluster");
  EXPECT_THROW(ClientHandshake(k, node("a"), 4095, Lane::control), std::invalid_argument);
  EXPECT_THROW(ClientHandshake(k, node("a"), 4 * 1024 * 1024 + 1, Lane::control), std::invalid_argument);
  EXPECT_THROW(server_handshake(k, node("b"), 1024, body(ClientHandshake(k, node("a"), 4096, Lane::control).hello_blob()), {"h", 1}), std::invalid_argument);
}

TEST(Channel, AnotherClustersKeyFailsBothWays) {
  const auto ours = key("ours");
  const auto theirs = key("theirs");
  ClientHandshake client(theirs, node("a"), 65536, Lane::control);
  EXPECT_THROW(server_handshake(ours, node("b"), 65536, body(client.hello_blob()), {"h", 1}), HandshakeRefused);
  ClientHandshake ours_client(ours, node("a"), 65536, Lane::control);
  auto reply = server_handshake(ours, node("b"), 65536, body(ours_client.hello_blob()), {"h", 1});
  ClientHandshake theirs_client(theirs, node("a"), 65536, Lane::control);
  EXPECT_THROW(theirs_client.finish(body(reply.response_blob)), HandshakeRefused);
}

TEST(Channel, AlteredHandshakeBytesAreRefused) {
  const auto k = key("cluster");
  ClientHandshake client(k, node("a"), 65536, Lane::control);
  auto hello = client.hello_blob();
  hello[10] ^= 1;
  EXPECT_THROW(server_handshake(k, node("b"), 65536, body(hello), {"h", 1}), HandshakeRefused);
  auto reply = server_handshake(k, node("b"), 65536, body(client.hello_blob()), {"h", 1});
  reply.response_blob[20] ^= 1;
  EXPECT_THROW(client.finish(body(reply.response_blob)), HandshakeRefused);
}

// Dannan's wire starts at version 1.
TEST(Channel, TheWireVersionStartsAtOne) {
  EXPECT_EQ(min_wire_version, 1);
  EXPECT_EQ(max_wire_version, 1);
  const auto k = key("cluster");
  const auto pair = handshake(k, 65536, k, 65536);
  EXPECT_EQ(pair.client.version, 1);
  EXPECT_EQ(pair.server.version, 1);
}

// The server picks the highest version both speak; with none in common the
// hello is refused, and nothing goes below either side's minimum.
TEST(Channel, TheHighestCommonVersionIsChosenAndNoneIsRefused) {
  const auto k = key("cluster");
  const auto agree = [&](WireVersions client, WireVersions server) {
    ClientHandshake hello(k, node("a"), 65536, Lane::control, Purpose::ordinary, {}, client);
    auto reply = server_handshake(k, node("b"), 65536, body(hello.hello_blob()), {"h", 1}, nullptr, server);
    const auto established = hello.finish(body(reply.response_blob));
    EXPECT_EQ(established.version, reply.session.version);
    return established.version;
  };
  EXPECT_EQ(agree({1, 3}, {2, 5}), 3);
  EXPECT_EQ(agree({2, 5}, {1, 3}), 3);
  EXPECT_EQ(agree({1, 1}, {1, 4}), 1);
  ClientHandshake old(k, node("a"), 65536, Lane::control, Purpose::ordinary, {}, {1, 2});
  EXPECT_THROW(server_handshake(k, node("b"), 65536, body(old.hello_blob()), {"h", 1}, nullptr, {3, 4}), HandshakeRefused);
  EXPECT_THROW(ClientHandshake(k, node("a"), 65536, Lane::control, Purpose::ordinary, {}, {2, 1}), std::invalid_argument);
  EXPECT_THROW(ClientHandshake(k, node("a"), 65536, Lane::control, Purpose::ordinary, {}, {0, 1}), std::invalid_argument);
}

// The ack tells the client where the server's socket saw it come from:
// how a node learns its candidate addresses without a STUN server.
TEST(Channel, TheAckCarriesTheObservedAddress) {
  const auto k = key("cluster");
  const auto pair = handshake(k, 65536, k, 65536);
  EXPECT_EQ(pair.client.observed, (Endpoint{"192.0.2.7", 50123}));
  EXPECT_EQ(pair.client.purpose, Purpose::ordinary);
}

// A punched or relayed connection proves itself inside the hello: the
// acceptor expecting one accepts only that purpose, that token and that
// peer, and a listening server refuses any such hello outright.
TEST(Channel, APathTokenIsCheckedInsideTheHandshake) {
  const auto k = key("cluster");
  const auto a = node("10.0.0.1");
  const PathToken token{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const Expected expected{Purpose::punched, token, a.id};
  {
    ClientHandshake hello(k, a, 65536, Lane::control, Purpose::punched, token);
    auto reply = server_handshake(k, node("b"), 65536, body(hello.hello_blob()), {"h", 1}, &expected);
    EXPECT_EQ(reply.session.purpose, Purpose::punched);
    EXPECT_EQ(hello.finish(body(reply.response_blob)).purpose, Purpose::punched);
  }
  {
    ClientHandshake hello(k, a, 65536, Lane::control, Purpose::punched, token);
    EXPECT_THROW(server_handshake(k, node("b"), 65536, body(hello.hello_blob()), {"h", 1}), HandshakeRefused);
  }
  auto wrong = token;
  wrong[0] ^= 1;
  for (const auto& [purpose, sent, from] :
       {std::tuple{Purpose::punched, wrong, a}, std::tuple{Purpose::relayed, token, a}, std::tuple{Purpose::punched, token, node("10.0.0.9")}}) {
    ClientHandshake hello(k, from, 65536, Lane::control, purpose, sent);
    EXPECT_THROW(server_handshake(k, node("b"), 65536, body(hello.hello_blob()), {"h", 1}, &expected), HandshakeRefused);
  }
  ClientHandshake ordinary(k, a, 65536, Lane::control);
  EXPECT_THROW(server_handshake(k, node("b"), 65536, body(ordinary.hello_blob()), {"h", 1}, &expected), HandshakeRefused);
  EXPECT_THROW(ClientHandshake(k, a, 65536, Lane::control, Purpose::ordinary, token), std::invalid_argument);
}

Pair established() {
  const auto k = key("cluster");
  return handshake(k, 65536, k, 65536);
}

WireFragment roundtrip(FrameSealer& sealer, FrameOpener& opener, uint64_t id, FrameType type, MessageType message, Bytes payload) {
  const auto wire = sealer.seal(id, type, message, true, true, payload);
  const auto rest = opener.header(std::span<const uint8_t, frame_header_size>(wire.data(), frame_header_size));
  EXPECT_EQ(rest + frame_header_size, wire.size());
  return opener.open(std::span(wire).subspan(frame_header_size));
}

TEST(Frames, CarryTheirHeaderAndPayloadAcross) {
  const auto pair = established();
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  FrameSealer sealer(pair.client, routes);
  FrameOpener opener(pair.server, routes);
  const auto fragment = roundtrip(sealer, opener, 7, FrameType::control, message::members, Bytes{1, 2, 3});
  EXPECT_EQ(fragment.request_id, 7u);
  EXPECT_EQ(fragment.frame, FrameType::control);
  EXPECT_EQ(fragment.type, message::members);
  EXPECT_TRUE(fragment.first);
  EXPECT_TRUE(fragment.last);
  EXPECT_EQ(fragment.payload, Bytes({1, 2, 3}));
}

// A 28-byte header, a 12-byte nonce and a 16-byte tag around the payload;
// the header opens with Dannan's magic, "DN" and the wire's number.
TEST(Frames, OpenWithDannansMagic) {
  const auto pair = established();
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  FrameSealer sealer(pair.client, routes);
  const auto wire = sealer.seal(1, FrameType::control, message::ping, true, true, Bytes{});
  EXPECT_EQ(Bytes(wire.begin(), wire.begin() + 4), (Bytes{'D', 'N', 0, 1}));
}

// The overhead: header, nonce and tag.
TEST(Frames, CostFiftySixBytesOfOverhead) {
  const auto pair = established();
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  FrameSealer sealer(pair.client, routes);
  EXPECT_EQ(sealer.seal(1, FrameType::control, message::ping, true, true, Bytes(100, 0)).size(), 156u);
  EXPECT_EQ(frame_overhead, 56u);
}

TEST(Frames, NothingLargerThanTheNegotiatedLimitIsSent) {
  const auto k = key("cluster");
  const auto pair = handshake(k, 4096, k, 4096);
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  FrameSealer sealer(pair.client, routes);
  EXPECT_THROW(sealer.seal(1, FrameType::control, message::ping, true, true, Bytes(4097, 0)), std::invalid_argument);
  EXPECT_NO_THROW(sealer.seal(1, FrameType::control, message::ping, true, true, Bytes(4096, 0)));
}

TEST(Frames, ATamperedFragmentIsRefused) {
  const auto pair = established();
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  for (const size_t at : {size_t{27}, size_t{60}}) {
    FrameSealer sealer(pair.client, routes);
    FrameOpener opener(pair.server, routes);
    auto wire = sealer.seal(1, FrameType::control, message::members, true, true, Bytes(8, 1));
    wire[at] ^= 1;
    EXPECT_THROW(
        {
          opener.header(std::span<const uint8_t, frame_header_size>(wire.data(), frame_header_size));
          opener.open(std::span(wire).subspan(frame_header_size));
        },
        FrameRefused)
        << at;
  }
}

TEST(Frames, AReplayedFragmentIsRefused) {
  const auto pair = established();
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  FrameSealer sealer(pair.client, routes);
  FrameOpener opener(pair.server, routes);
  const auto wire = sealer.seal(1, FrameType::control, message::members, true, true, Bytes{9});
  opener.header(std::span<const uint8_t, frame_header_size>(wire.data(), frame_header_size));
  opener.open(std::span(wire).subspan(frame_header_size));
  EXPECT_THROW(opener.header(std::span<const uint8_t, frame_header_size>(wire.data(), frame_header_size)), FrameRefused);
}

// The receiver, not the sender's lane choice, keeps object traffic off the
// control lane: the opener refuses on the header.
TEST(Frames, AMessageOffItsLaneOrClassIsRefusedOnItsHeader) {
  const auto pair = established();
  auto table = RouteTable::with_dannan_types();
  table.add(MessageType{1028}, {"get_object", Lanes::data, Frames::not_control, false});
  const auto routes = std::make_shared<const RouteTable>(std::move(table));
  // The sender's table has never heard of the rule; the receiver's has.
  FrameSealer sealer(pair.client, std::make_shared<const RouteTable>(RouteTable::with_dannan_types()));
  FrameOpener opener(pair.server, routes);
  const auto wire = sealer.seal(1, FrameType::control, MessageType{1028}, true, true, Bytes{1});
  EXPECT_THROW(opener.header(std::span<const uint8_t, frame_header_size>(wire.data(), frame_header_size)), FrameRefused);
}

TEST(Frames, TheSenderRefusesWhatItsOwnTableForbids) {
  const auto pair = established();
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  FrameSealer sealer(pair.client, routes);
  EXPECT_THROW(sealer.seal(1, FrameType::loader, message::members, true, true, Bytes{1}), std::invalid_argument);
}

TEST(Frames, CountersAdvanceOnEachSide) {
  const auto pair = established();
  const auto routes = std::make_shared<const RouteTable>(RouteTable::with_dannan_types());
  FrameSealer sealer(pair.client, routes);
  FrameOpener opener(pair.server, routes);
  for (uint64_t i = 1; i <= 5; ++i) roundtrip(sealer, opener, i, FrameType::control, message::ping, Bytes{});
  EXPECT_EQ(sealer.counter(), 5u);
  EXPECT_EQ(opener.counter(), 5u);
}

}  // namespace
}  // namespace dannan::net
