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

#include <algorithm>
#include <fstream>
#include <functional>
#include <tuple>

#include "dannan/net/channel.h"
#include "dannan/test/fixtures/stepped_time.h"
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

// Dannan's wire starts at version 1: a cluster key proves version 1, a
// certificate version 2 (TODO/2026-11-04-identity.md).
TEST(Channel, TheWireVersionStartsAtOne) {
  EXPECT_EQ(min_wire_version, 1);
  EXPECT_EQ(max_wire_version, 2);
  const auto k = key("cluster");
  const auto pair = handshake(k, 65536, k, 65536);
  EXPECT_EQ(pair.client.version, 1);
  EXPECT_EQ(pair.server.version, 1);
  EXPECT_FALSE(pair.client.peer_certificate);
}

TEST(Channel, AHelloWithNoCommonVersionIsRefused) {
  const auto k = key("cluster");
  ClientHandshake client(k, node("10.0.0.1"), 65536, Lane::control);
  try {
    (void)server_handshake(k, node("10.0.0.2"), 65536, body(client.hello_blob()), {"192.0.2.7", 1}, nullptr, WireVersions{5, 6});
    ADD_FAILURE() << "a hello below our range was answered";
  } catch (const HandshakeRefused& refused) {
    EXPECT_NE(std::string(refused.what()).find("no common wire version"), std::string::npos) << refused.what();
  }
}

// An ack correctly authenticated by a key holder but saying what no honest
// server would: `edit` changes it, and the MAC is made afresh.
std::string forged_ack_refusal(const std::function<void(Bytes&)>& edit, size_t client_frame = 65536) {
  const auto k = key("cluster");
  ClientHandshake client(k, node("10.0.0.1"), client_frame, Lane::control);
  const auto reply = server_handshake(k, node("10.0.0.2"), 65536, body(client.hello_blob()), {"192.0.2.7", 1});
  Reader hello_envelope(body(client.hello_blob()));
  const auto hello = hello_envelope.bytes(max_handshake_blob);
  Reader envelope(body(reply.response_blob));
  auto ack = envelope.bytes(max_handshake_blob);
  edit(ack);
  Bytes material;
  const std::string label = "dannan/ack/1";
  material.insert(material.end(), label.begin(), label.end());
  material.insert(material.end(), hello.begin(), hello.end());
  material.insert(material.end(), ack.begin(), ack.end());
  Writer forged;
  forged.bytes(ack);
  forged.fixed(identity::hmac_sha256(k.auth, material));
  const auto bytes = forged.take();
  try {
    (void)client.finish(bytes);
  } catch (const HandshakeRefused& refused) {
    return refused.what();
  }
  return {};
}

TEST(Channel, AnAckOutsideTheRangeIsRefused) {
  const auto why = forged_ack_refusal([](Bytes& ack) {
    ack[0] = 0;
    ack[1] = 9;
  });
  EXPECT_NE(why.find("outside our range"), std::string::npos) << why;
  const auto zero = forged_ack_refusal([](Bytes& ack) {
    ack[0] = 0;
    ack[1] = 0;
  });
  EXPECT_NE(zero.find("outside our range"), std::string::npos) << zero;
}

TEST(Channel, ANegotiatedFrameAboveTheLimitIsRefused) {
  // The client offered 16 KiB; the forged ack "negotiates" 32 KiB.
  const auto why = forged_ack_refusal(
      [](Bytes& ack) {
        ack[2] = 0;
        ack[3] = 0;
        ack[4] = 0x80;
        ack[5] = 0;
      },
      16384);
  EXPECT_NE(why.find("invalid frame size"), std::string::npos) << why;
}

constexpr uint64_t now_ms = 1'800'000'000'000;

// A cluster's CA and a node's certificate credential under it.
struct Cluster {
  identity::CertificateAuthority ca = identity::CertificateAuthority::create("test", now_ms);
  test::SteppedWall wall{now_ms};
  std::vector<identity::Revocation> revoked;
  uint64_t serial = 1;

  CertificateCredential credential(const membership::NodeRecord& node) {
    auto key = identity::SigningKey::generate(identity::SigningAlgorithm::es256);
    auto own = ca.issue(node.id, key.public_key(), serial++, now_ms);
    CertificateCredential out{ca.certificate(), std::move(own), std::move(key), {}, &wall};
    out.revoked = [this](const identity::Certificate& c) {
      return std::any_of(revoked.begin(), revoked.end(), [&](const identity::Revocation& r) { return r.covers(c); });
    };
    return out;
  }
};

Pair handshake(const Credentials& client, const membership::NodeRecord& a, const Credentials& server, const membership::NodeRecord& b) {
  ClientHandshake hello(client, a, 65536, Lane::control);
  auto reply = server_handshake(server, b, 65536, body(hello.hello_blob()), {"192.0.2.7", 1});
  auto established = hello.finish(body(reply.response_blob));
  return {std::move(established), std::move(reply.session)};
}

// Two nodes of one CA meet on version 2: each checks the other's
// certificate and keeps it; the session works.
TEST(Channel, CertificatesProveVersionTwo) {
  Cluster cluster;
  const auto a = node("10.0.0.1");
  const auto b = node("10.0.0.2");
  const auto pair = handshake(Credentials(cluster.credential(a)), a, Credentials(cluster.credential(b)), b);
  EXPECT_EQ(pair.client.version, 2);
  EXPECT_EQ(pair.server.version, 2);
  ASSERT_TRUE(pair.client.peer_certificate && pair.server.peer_certificate);
  EXPECT_EQ(pair.client.peer_certificate->node_id(), b.id);
  EXPECT_EQ(pair.server.peer_certificate->node_id(), a.id);
  EXPECT_EQ(pair.client.keys.tx, pair.server.keys.rx);
  EXPECT_EQ(pair.client.keys.rx, pair.server.keys.tx);
}

// A cluster moving node by node: a node with both a key and a certificate
// meets one with either; two with both use version 2; a node with only a
// certificate and one with only a key share nothing.
TEST(Channel, ANodeWithBothMeetsEither) {
  Cluster cluster;
  const auto k = key("cluster");
  const auto a = node("10.0.0.1");
  const auto b = node("10.0.0.2");
  const auto both = [&](const membership::NodeRecord& n) {
    Credentials out(k);
    out.certificate = cluster.credential(n);
    return out;
  };
  EXPECT_EQ(handshake(both(a), a, both(b), b).client.version, 2);
  EXPECT_EQ(handshake(both(a), a, Credentials(k), b).client.version, 1);
  EXPECT_EQ(handshake(Credentials(k), a, both(b), b).client.version, 1);
  EXPECT_EQ(handshake(both(a), a, Credentials(cluster.credential(b)), b).client.version, 2);
  ClientHandshake certificate_only(Credentials(cluster.credential(a)), a, 65536, Lane::control);
  EXPECT_THROW(server_handshake(Credentials(k), b, 65536, body(certificate_only.hello_blob()), {"h", 1}), HandshakeRefused);
  EXPECT_THROW(ClientHandshake(Credentials(k), a, 65536, Lane::control, Purpose::ordinary, {}, WireVersions{2, 2}), std::invalid_argument);
  EXPECT_THROW(ClientHandshake(k, node("a"), 65536, Lane::control, Purpose::ordinary, {}, WireVersions{2, 1}), std::invalid_argument);
  EXPECT_THROW(Credentials().versions(), std::logic_error);
}

// What a certificate must be, checked inside the handshake both ways:
// this cluster's CA, the receiver's clock, the node the hello names, not
// revoked.
TEST(Channel, ACertificateIsCheckedBothWays) {
  Cluster cluster;
  Cluster other;
  const auto a = node("10.0.0.1");
  const auto b = node("10.0.0.2");
  const Credentials server(cluster.credential(b));
  // Another cluster's node.
  EXPECT_THROW(handshake(Credentials(other.credential(a)), a, server, b), HandshakeRefused);
  // A certificate naming another node than the hello's.
  auto stolen = cluster.credential(node("10.0.0.9"));
  EXPECT_THROW(handshake(Credentials(stolen), a, server, b), HandshakeRefused);
  // Revoked, by serial or for the whole node.
  auto credential = cluster.credential(a);
  const auto serial = credential.own.serial();
  cluster.revoked.push_back(cluster.ca.revoke(a.id, serial, now_ms, "lost"));
  EXPECT_THROW(handshake(Credentials(credential), a, server, b), HandshakeRefused);
  cluster.revoked.clear();
  EXPECT_NO_THROW(handshake(Credentials(credential), a, server, b));
  // Expired at the receiver's clock.
  cluster.wall.advance(std::chrono::hours(24 * 365 * 3));
  EXPECT_THROW(handshake(Credentials(cluster.credential(a)), a, server, b), HandshakeRefused);
  cluster.wall.advance(-std::chrono::hours(24 * 365 * 3));
  // And the client checks the server's the same way.
  EXPECT_THROW(handshake(Credentials(cluster.credential(a)), a, Credentials(other.credential(b)), b), HandshakeRefused);
}

// The signature binds the hello: an altered byte is refused.
TEST(Channel, ACertificateHelloCannotBeAltered) {
  Cluster cluster;
  const auto a = node("10.0.0.1");
  const auto b = node("10.0.0.2");
  ClientHandshake hello(Credentials(cluster.credential(a)), a, 65536, Lane::control);
  auto bent = hello.hello_blob();
  bent[30] ^= 1;
  EXPECT_THROW(server_handshake(Credentials(cluster.credential(b)), b, 65536, body(bent), {"h", 1}), HandshakeRefused);
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
