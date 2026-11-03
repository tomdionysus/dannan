//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Dannan against Macha's own code in one process: whatever protocol 23
// says, both sides compute it the same way. Macha is read-only here, a
// pinned export of a tagged release.

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <fstream>
#include <future>
#include <map>
#include <random>
#include <thread>

#include "cluster/membership.hpp"
#include "cluster/net.hpp"
#include "cluster/placement.hpp"
#include "cluster/telemetry.hpp"
#include "codec.hpp"
#include "crypto.hpp"
#include "dannan/identity/cluster_key.h"
#include "dannan/membership/membership.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/channel.h"
#include "dannan/net/session.h"
#include "dannan/placement/placement.h"
#include "dannan/status/observation.h"
#include "dannan/status/telemetry.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "observation.hpp"
#include "types.hpp"

namespace dannan {
namespace {

// The same key file is the same cluster to both.
TEST(Interop, TheClusterKeyDerivesAsMachasDoes) {
  test::TempDir dir;
  for (const auto* secret : {"one secret\n", "another\r\n\n", "x"}) {
    const auto path = dir.path() / "cluster.key";
    std::ofstream(path, std::ios::binary | std::ios::trunc) << secret;
    const auto ours = identity::load_cluster_key(path);
    const auto theirs = macha::load_cluster_keys(path);
    EXPECT_EQ(ours.master, theirs.master);
    EXPECT_EQ(ours.auth, theirs.auth);
    EXPECT_EQ(ours.cluster_id, theirs.cluster_id);
    EXPECT_EQ(ours.derive("macha/storage/v1"), theirs.storage);
  }
}

membership::NodeRecord ours_sample() {
  membership::NodeRecord node;
  node.id = identity::NodeId::random();
  node.host = "media-node-2.example.net";
  node.failure_domain = "site-a";
  node.port = 7437;
  node.capacity = 10ull << 40;
  node.used = 3ull << 30;
  node.seen_unix_ms = 1793700000000ull;
  node.metadata_generation = 4242;
  node.metadata_write_replicas_required = 2;
  node.flags = membership::flags_for(false, true);
  return node;
}

macha::NodeInfo theirs_of(const membership::NodeRecord& ours) {
  macha::NodeInfo node;
  node.id.bytes = ours.id.bytes;
  node.host = ours.host;
  node.failure_domain = ours.failure_domain;
  node.port = ours.port;
  node.capacity = ours.capacity;
  node.used = ours.used;
  node.seen_unix_ms = ours.seen_unix_ms;
  node.metadata_generation = ours.metadata_generation;
  node.metadata_write_replicas_required = ours.metadata_write_replicas_required;
  node.flags = ours.flags;
  return node;
}

// The record every handshake and every roster exchange carries.
TEST(Interop, TheNodeRecordEncodesAsMachasDoes) {
  const auto ours = ours_sample();
  net::Writer writer;
  membership::encode(writer, ours);
  macha::Writer theirs;
  macha::encode_node_info(theirs, theirs_of(ours));
  EXPECT_EQ(writer.data(), theirs.data());

  macha::Reader reader(theirs.data());
  const auto decoded = macha::decode_node_info(reader);
  net::Reader back(writer.data());
  const auto mine = membership::decode(back);
  EXPECT_EQ(mine.host, decoded.host);
  EXPECT_EQ(mine.id.bytes, decoded.id.bytes);
  EXPECT_EQ(mine.flags, decoded.flags);
}

void write_all(int fd, std::span<const uint8_t> bytes) {
  size_t done = 0;
  while (done < bytes.size()) {
    const auto n = ::send(fd, bytes.data() + done, bytes.size() - done, 0);
    ASSERT_GT(n, 0);
    done += static_cast<size_t>(n);
  }
}

net::Bytes read_exact(int fd, size_t size) {
  net::Bytes out(size);
  size_t done = 0;
  while (done < size) {
    const auto n = ::recv(fd, out.data() + done, size - done, 0);
    if (n <= 0) throw std::runtime_error("peer closed");
    done += static_cast<size_t>(n);
  }
  return out;
}

// One length-prefixed handshake blob's body.
net::Bytes read_blob(int fd) {
  const auto length = read_exact(fd, 4);
  net::Reader reader(length);
  return read_exact(fd, reader.u32());
}

// One sealed frame through Dannan's opener.
net::WireFragment read_frame(int fd, net::FrameOpener& opener) {
  const auto header = read_exact(fd, net::frame_header_size);
  const auto rest = opener.header(std::span<const uint8_t, net::frame_header_size>(header.data(), net::frame_header_size));
  return opener.open(read_exact(fd, rest));
}

struct Socketpair {
  int ours = -1;
  int theirs = -1;
  Socketpair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) throw std::runtime_error("socketpair");
    ours = fds[0];
    theirs = fds[1];
  }
  ~Socketpair() {
    if (ours >= 0) ::close(ours);
  }
};

// Macha's message types for the cases below, registered as protocol 23 has
// them, so Dannan's table makes the same decisions Macha's does.
std::shared_ptr<const net::RouteTable> routes() {
  auto table = net::RouteTable::with_dannan_types();
  table.add(net::MessageType{5}, {"put_object", net::Lanes::data, net::Frames::not_control, false});
  table.add(net::MessageType{4}, {"get_object", net::Lanes::data, net::Frames::not_control, false});
  table.add(net::MessageType{104}, {"object_reply", net::Lanes::data, net::Frames::not_control, false});
  return std::make_shared<const net::RouteTable>(std::move(table));
}

struct Keys {
  test::TempDir dir;
  identity::ClusterKey ours;
  macha::ClusterKeys theirs;
  Keys() {
    std::ofstream(dir.path() / "cluster.key") << "interop cluster";
    ours = identity::load_cluster_key(dir.path() / "cluster.key");
    theirs = macha::load_cluster_keys(dir.path() / "cluster.key");
  }
};

// A Dannan client and a Macha server: the handshake completes, both ends hold
// the same session, and frames cross both ways and are checked by each
// side's own rules.
TEST(Interop, ADannanClientTalksToAMachaServer) {
  Keys keys;
  Socketpair pair;
  const auto mine = ours_sample();
  auto server_node = theirs_of(ours_sample());
  server_node.host = "127.0.0.2";
  macha::SecureChannel server(pair.theirs, keys.theirs, server_node, 65536);
  macha::NodeInfo seen;
  std::exception_ptr failed;
  std::thread server_side([&] {
    try {
      seen = server.server_handshake("127.0.0.9");
    } catch (...) {
      failed = std::current_exception();
    }
  });
  net::ClientHandshake client(keys.ours, mine, 16384, net::Lane::control);
  write_all(pair.ours, client.hello_blob());
  const auto established = client.finish(read_blob(pair.ours));
  server_side.join();
  ASSERT_FALSE(failed);
  EXPECT_EQ(seen.id.bytes, mine.id.bytes);
  EXPECT_EQ(seen.host, mine.host);
  EXPECT_EQ(established.peer.id.bytes, server_node.id.bytes);
  EXPECT_EQ(established.max_frame, 16384u);
  EXPECT_EQ(server.max_frame_size(), 16384u);
  EXPECT_EQ(established.keys.session_id, server.session_id());

  const auto table = routes();
  net::FrameSealer sealer(established, table);
  net::FrameOpener opener(established, table);
  write_all(pair.ours, sealer.seal(3, sched::FrameType::control, net::message::members, true, true, net::Bytes{1, 2, 3}));
  const auto arrived = server.receive_fragment();
  EXPECT_EQ(arrived.request_id, 3u);
  EXPECT_EQ(arrived.message_type, macha::MessageType::members);
  EXPECT_EQ(arrived.frame_type, macha::FrameType::control);
  EXPECT_EQ(arrived.payload, net::Bytes({1, 2, 3}));
  // Telemetry is gossiped as speculative work on the control lane.
  const auto telemetry = sealer.seal(0, sched::FrameType::speculative, net::message::telemetry, true, true, net::Bytes(5000, 7));
  std::thread writer([&] { write_all(pair.ours, telemetry); });
  EXPECT_EQ(server.receive_fragment().payload.size(), 5000u);
  writer.join();

  server.send_fragment(3, macha::FrameType::control, macha::MessageType::members_reply, true, true, macha::Bytes{4, 5});
  const auto reply = read_frame(pair.ours, opener);
  EXPECT_EQ(reply.request_id, 3u);
  EXPECT_EQ(reply.type, net::message::members_reply);
  EXPECT_EQ(reply.payload, net::Bytes({4, 5}));
  // A socketpair buffers a few KiB: large writes run beside the reader.
  std::thread sender([&] {
    server.send_fragment(3, macha::FrameType::control, macha::MessageType::ok, true, false, macha::Bytes(16384, 1));
    server.send_fragment(3, macha::FrameType::control, macha::MessageType::ok, false, true, macha::Bytes(10, 2));
  });
  EXPECT_FALSE(read_frame(pair.ours, opener).last);
  EXPECT_TRUE(read_frame(pair.ours, opener).last);
  sender.join();
  ::shutdown(pair.ours, SHUT_RDWR);
}

// A Macha client and a Dannan server, on the DATA lane, with object traffic.
TEST(Interop, AMachaClientTalksToADannanServerOnTheDataLane) {
  Keys keys;
  Socketpair pair;
  auto client_node = theirs_of(ours_sample());
  macha::SecureChannel client(pair.theirs, keys.theirs, client_node, 262144);
  macha::NodeInfo seen;
  std::exception_ptr failed;
  std::thread client_side([&] {
    try {
      seen = client.client_handshake(macha::TransportLane::data);
    } catch (...) {
      failed = std::current_exception();
    }
  });
  const auto mine = ours_sample();
  auto reply = net::server_handshake(keys.ours, mine, 65536, read_blob(pair.ours), "127.0.0.9");
  write_all(pair.ours, reply.response_blob);
  client_side.join();
  ASSERT_FALSE(failed);
  EXPECT_EQ(seen.id.bytes, mine.id.bytes);
  EXPECT_EQ(reply.session.lane, net::Lane::data);
  EXPECT_EQ(reply.session.max_frame, 65536u);
  EXPECT_EQ(client.max_frame_size(), 65536u);
  EXPECT_EQ(reply.session.keys.session_id, client.session_id());
  EXPECT_EQ(reply.session.peer.id.bytes, client_node.id.bytes);

  const auto table = routes();
  net::FrameSealer sealer(reply.session, table);
  net::FrameOpener opener(reply.session, table);
  client.send_fragment(9, macha::FrameType::foreground, macha::MessageType::get_object, true, true, macha::Bytes(32, 0xaa));
  const auto request = read_frame(pair.ours, opener);
  EXPECT_EQ(request.type, net::MessageType{4});
  EXPECT_EQ(request.frame, sched::FrameType::foreground);
  const auto sealed = sealer.seal(9, sched::FrameType::foreground, net::MessageType{104}, true, true, net::Bytes(60000, 0xbb));
  std::thread writer([&] { write_all(pair.ours, sealed); });
  const auto object = client.receive_fragment();
  writer.join();
  EXPECT_EQ(object.message_type, macha::MessageType::object_reply);
  EXPECT_EQ(object.payload, macha::Bytes(60000, 0xbb));
  // ping rides the DATA lane both ways.
  client.send_fragment(10, macha::FrameType::control, macha::MessageType::ping, true, true, {});
  EXPECT_EQ(read_frame(pair.ours, opener).type, net::message::ping);
  ::shutdown(pair.ours, SHUT_RDWR);
}

// Macha refuses on its own rules what Dannan's table would also refuse:
// object traffic on a control session ends it.
TEST(Interop, MachaRefusesObjectTrafficOnAControlSessionAsDannanDoes) {
  Keys keys;
  Socketpair pair;
  auto server_node = theirs_of(ours_sample());
  macha::SecureChannel server(pair.theirs, keys.theirs, server_node, 65536);
  std::thread server_side([&] { server.server_handshake("127.0.0.9"); });
  net::ClientHandshake client(keys.ours, ours_sample(), 65536, net::Lane::control);
  write_all(pair.ours, client.hello_blob());
  const auto established = client.finish(read_blob(pair.ours));
  server_side.join();
  // A permissive sender, so the receiver's rule is what is tested.
  auto permissive = net::RouteTable::with_dannan_types();
  permissive.add(net::MessageType{4}, {"get_object", net::Lanes::both, net::Frames::any, false});
  net::FrameSealer sealer(established, std::make_shared<const net::RouteTable>(std::move(permissive)));
  write_all(pair.ours, sealer.seal(1, sched::FrameType::foreground, net::MessageType{4}, true, true, net::Bytes{1}));
  EXPECT_THROW(server.receive_fragment(), std::runtime_error);
  EXPECT_FALSE(routes()->allowed(net::MessageType{4}, net::Lane::control, sched::FrameType::foreground));
  ::shutdown(pair.ours, SHUT_RDWR);
}

// Dannan's Asio session in both roles against Macha's channel, with real
// request ids: the dialler's odd, the acceptor's even, and fragmented replies.
TEST(Interop, DannansSessionAnswersAndCallsAMachaPeer) {
  namespace asio = boost::asio;
  Keys keys;
  asio::io_context io;
  auto work = asio::make_work_guard(io);
  std::thread runner([&] { io.run(); });
  Socketpair pair;
  // Macha dials; Dannan accepts.
  auto client_node = theirs_of(ours_sample());
  macha::SecureChannel macha_side(pair.theirs, keys.theirs, client_node, 4096);
  std::thread macha_handshake([&] { macha_side.client_handshake(macha::TransportLane::control); });
  net::Session::Socket socket(io, net::Session::Socket::protocol_type(AF_UNIX, SOCK_STREAM), pair.ours);
  pair.ours = -1;  // the socket owns it now
  std::promise<net::Established> accepted;
  net::async_accept_handshake(socket, keys.ours, ours_sample(), 65536, "127.0.0.9", [&](std::exception_ptr error, net::Established done) {
    if (error)
      accepted.set_exception(error);
    else
      accepted.set_value(std::move(done));
  });
  auto established = accepted.get_future().get();
  macha_handshake.join();
  EXPECT_EQ(established.max_frame, 4096u);

  net::SessionHandlers handlers;
  handlers.request = [](net::Frame frame, net::Reply reply) { reply({net::message::members_reply, net::Bytes(10000, 0x42), {}}); };
  auto session = net::Session::create(std::move(socket), std::move(established), net::Role::acceptor, routes(), std::move(handlers));
  session->start();

  // Macha asks with an odd id; Dannan's reply comes back in 4 KiB fragments.
  macha_side.send_fragment(7, macha::FrameType::control, macha::MessageType::members, true, true, macha::Bytes{1});
  macha::Bytes reassembled;
  for (;;) {
    const auto fragment = macha_side.receive_fragment();
    EXPECT_EQ(fragment.request_id, 7u);
    EXPECT_EQ(fragment.message_type, macha::MessageType::members_reply);
    reassembled.insert(reassembled.end(), fragment.payload.begin(), fragment.payload.end());
    if (fragment.last) break;
  }
  EXPECT_EQ(reassembled, macha::Bytes(10000, 0x42));

  // Dannan asks with an even id; Macha answers it.
  std::promise<net::CallOutcome> outcome;
  const auto admitted = session->call(net::message::ping, net::Bytes{9}, sched::FrameType::control, io.get_executor(),
                                      [&](net::CallOutcome result) { outcome.set_value(std::move(result)); });
  ASSERT_TRUE(admitted.refused.empty());
  EXPECT_EQ(admitted.request_id % 2, 0u);
  const auto request = macha_side.receive_fragment();
  EXPECT_EQ(request.request_id, admitted.request_id);
  EXPECT_EQ(request.message_type, macha::MessageType::ping);
  macha_side.send_fragment(request.request_id, macha::FrameType::control, macha::MessageType::ok, true, true, macha::Bytes{9});
  const auto result = outcome.get_future().get();
  ASSERT_TRUE(result.reply);
  EXPECT_EQ(result.reply->type, net::message::ok);
  EXPECT_EQ(result.reply->payload, net::Bytes{9});

  session->close("done");
  work.reset();
  io.stop();
  runner.join();
}

// A capture of one session: every frame Macha's channel writes is the frame
// Dannan's sealer writes for the same fragment, byte for byte, given the
// nonce (the one random part of a frame, read from Macha's).
TEST(Interop, DannansFramesAreMachasByteForByte) {
  Keys keys;
  Socketpair pair;
  auto server_node = theirs_of(ours_sample());
  macha::SecureChannel server(pair.theirs, keys.theirs, server_node, 65536);
  std::thread server_side([&] { (void)server.server_handshake("127.0.0.9"); });
  net::ClientHandshake client(keys.ours, ours_sample(), 16384, net::Lane::control);
  write_all(pair.ours, client.hello_blob());
  const auto established = client.finish(read_blob(pair.ours));
  server_side.join();

  // Dannan sealing as the server: its sending key is the client's receiving one.
  auto as_server = established;
  as_server.keys.tx = established.keys.rx;
  net::FrameSealer sealer(as_server, routes());
  std::array<uint8_t, 12> next_nonce{};
  sealer.use_nonces_for_test([&] { return next_nonce; });

  struct Sent {
    uint64_t request_id;
    macha::FrameType theirs;
    sched::FrameType ours;
    uint16_t type;
    bool first;
    bool last;
    net::Bytes payload;
  };
  const std::vector<Sent> sent{
      {3, macha::FrameType::control, sched::FrameType::control, 102, true, true, net::Bytes{4, 5}},
      {0, macha::FrameType::speculative, sched::FrameType::speculative, 24, true, true, net::Bytes(3000, 7)},
      {5, macha::FrameType::control, sched::FrameType::control, 100, true, false, net::Bytes(16384, 1)},
      {5, macha::FrameType::control, sched::FrameType::control, 100, false, true, net::Bytes(10, 2)},
      {9, macha::FrameType::loader, sched::FrameType::loader, 101, true, true, net::Bytes{}},
      {0, macha::FrameType::control, sched::FrameType::control, 12, true, true, net::Bytes{}},
  };
  std::thread sender([&] {
    for (const auto& frame : sent)
      server.send_fragment(frame.request_id, frame.theirs, static_cast<macha::MessageType>(frame.type), frame.first, frame.last, frame.payload);
  });
  for (const auto& frame : sent) {
    auto captured = read_exact(pair.ours, net::frame_header_size);
    net::Reader header(captured);
    header.view(8);
    const auto size = header.u32();
    const auto rest = read_exact(pair.ours, 12 + 16 + size);
    captured.insert(captured.end(), rest.begin(), rest.end());
    std::copy(rest.begin(), rest.begin() + 12, next_nonce.begin());
    const auto ours = sealer.seal(frame.request_id, frame.ours, net::MessageType{frame.type}, frame.first, frame.last, frame.payload);
    EXPECT_EQ(ours, captured) << "type " << frame.type << " request " << frame.request_id;
  }
  sender.join();
  ::shutdown(pair.ours, SHUT_RDWR);
}

// The persisted roster, both ways: what one writes the other loads.
TEST(Interop, TheRosterFileIsSharedWithMacha) {
  test::TempDir dir;
  const auto ours_path = dir.path() / "ours";
  const auto theirs_path = dir.path() / "theirs";
  auto self = ours_sample();
  auto peer = ours_sample();
  peer.host = "10.1.2.3";
  auto other = ours_sample();
  other.host = "10.1.2.4";
  other.flags = membership::flags_default;
  membership::IdentityReset reset;
  reset.host = "10.9.9.9";
  reset.epoch = 3;
  reset.reset_unix_ms = 77;
  reset.reason = "replaced";
  {
    membership::Membership ours(self, std::chrono::seconds(30), ours_path);
    ours.observe(peer, true);
    ours.observe(other, true);
    ours.apply_identity_reset(reset);
  }
  macha::Membership theirs(theirs_of(self), std::chrono::seconds(30), ours_path);
  const auto loaded = theirs.all();
  ASSERT_EQ(loaded.size(), 3u);
  for (const auto& expected : {peer, other}) {
    const auto found = std::find_if(loaded.begin(), loaded.end(), [&](const macha::NodeInfo& n) { return n.id.bytes == expected.id.bytes; });
    ASSERT_NE(found, loaded.end());
    EXPECT_EQ(found->host, expected.host);
    EXPECT_EQ(found->flags, expected.flags);
    EXPECT_EQ(found->failure_domain, expected.failure_domain);
  }
  ASSERT_EQ(theirs.identity_resets().size(), 1u);
  EXPECT_EQ(theirs.identity_resets().front().reason, "replaced");

  {
    macha::Membership writer(theirs_of(self), std::chrono::seconds(30), theirs_path);
    writer.observe(theirs_of(peer), true);
  }
  membership::Membership back(self, std::chrono::seconds(30), theirs_path);
  const auto all = back.all();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[1].id, peer.id);
  EXPECT_EQ(all[1].flags, peer.flags);
}

// Placement decides as Macha's does, key for key, over random rosters: same
// owners in the same order, same fallbacks, same capacity, same commit order.
TEST(Interop, PlacementDecidesAsMachasDoes) {
  std::mt19937_64 random(20261103);
  const std::vector<std::string> domain_names{"", "", "a", "b", "c"};
  for (int round = 0; round < 400; ++round) {
    std::vector<membership::NodeRecord> ours;
    std::vector<macha::NodeInfo> theirs;
    const auto count = 1 + random() % 9;
    for (size_t i = 0; i < count; ++i) {
      auto n = ours_sample();
      n.capacity = random() % 4 == 0 ? 0 : random() % (1ull << 40);
      n.failure_domain = domain_names[random() % domain_names.size()];
      ours.push_back(n);
      theirs.push_back(theirs_of(n));
    }
    for (int k = 0; k < 20; ++k) {
      std::vector<uint8_t> key(k % 5 == 0 ? 3 : 32);
      for (auto& byte : key) byte = static_cast<uint8_t>(random());
      for (size_t replicas = 0; replicas <= 4; ++replicas) {
        const auto ours_by_capacity = placement::by_capacity(key, ours, replicas);
        const auto theirs_by_capacity = macha::capacity_placement_nodes(key, theirs, replicas);
        ASSERT_EQ(ours_by_capacity.size(), theirs_by_capacity.size());
        for (size_t i = 0; i < ours_by_capacity.size(); ++i) ASSERT_EQ(ours_by_capacity[i].id.bytes, theirs_by_capacity[i].id.bytes) << "round " << round;
        const auto ours_rendezvous = placement::rendezvous(key, ours, replicas);
        const auto theirs_rendezvous = macha::rendezvous_nodes(key, theirs, replicas);
        ASSERT_EQ(ours_rendezvous.size(), theirs_rendezvous.size());
        for (size_t i = 0; i < ours_rendezvous.size(); ++i) ASSERT_EQ(ours_rendezvous[i].id.bytes, theirs_rendezvous[i].id.bytes);
      }
    }
    for (size_t replicas = 0; replicas <= 4; ++replicas)
      ASSERT_EQ(placement::logical_capacity(ours, replicas), macha::placement_logical_capacity(theirs, replicas));
    std::map<identity::NodeId, std::chrono::milliseconds> rtt;
    for (const auto& n : ours)
      if (random() % 2) rtt[n.id] = std::chrono::milliseconds(random() % 5);
    const auto ours_order =
        placement::order_commit_replicas(ours, ours.front().id, [&](const identity::NodeId& id) -> std::optional<std::chrono::milliseconds> {
          const auto found = rtt.find(id);
          return found == rtt.end() ? std::nullopt : std::optional(found->second);
        });
    macha::NodeId local;
    local.bytes = ours.front().id.bytes;
    const auto theirs_order = macha::order_commit_replicas(theirs, local, [&](const macha::NodeId& id) -> std::optional<std::chrono::milliseconds> {
      identity::NodeId mine;
      mine.bytes = id.bytes;
      const auto found = rtt.find(mine);
      return found == rtt.end() ? std::nullopt : std::optional(found->second);
    });
    for (size_t i = 0; i < ours_order.size(); ++i) ASSERT_EQ(ours_order[i].id.bytes, theirs_order[i].id.bytes);
  }
}

// Telemetry, grandfathered: a full Macha record decodes in Dannan and
// re-encodes to the same bytes, every field Macha's kept as it came; and a
// record Dannan builds in protocol 23's layout reads in Macha as written.
TEST(Interop, TelemetryRecordsCrossBothWaysByteForByte) {
  macha::NodeTelemetry theirs;
  theirs.node_id.bytes = ours_sample().id.bytes;
  theirs.boot_id.bytes = ours_sample().id.bytes;
  theirs.sequence = 42;
  theirs.observed_unix_ms = 1793700000123ull;
  theirs.version = "0.91.2";
  theirs.host = "10.0.0.7";
  theirs.failure_domain = "site-a";
  theirs.port = 7437;
  theirs.storage_capacity = 10ull << 40;
  theirs.storage_used = 3ull << 30;
  theirs.cache_capacity = 1 << 20;
  theirs.cache_used = 1 << 10;
  theirs.metadata_generation = 99;
  theirs.uptime_ms = 5000;
  theirs.rss_bytes = 1 << 28;
  theirs.process_cpu_milli_percent = 1234;
  theirs.load1_milli = 900;
  theirs.storage_backends_online = 2;
  theirs.peers_known = 3;
  theirs.peers_active = 2;
  theirs.rpc_connections_created = 7;
  theirs.rpc_connections_reused = 70;
  theirs.rpc_connections_canonical = 4;
  theirs.phase = macha::NodePhase::recovering;
  theirs.api_endpoint = "http://10.0.0.7:7438";
  theirs.cpu_cores = 8;
  theirs.memory_total_bytes = 16ull << 30;
  theirs.playback_startup_timeout_ms = 15000;
  theirs.playback_segment_timeout_ms = 6000;
  theirs.playback_max_sessions = 64;
  theirs.playback_max_transcodes_per_account = 2;
  theirs.playback_start_failed_retention_ms = 60000;
  theirs.playback_transcode_rates.push_back({"video", "h264", 8, 1080, 1500, 12, 1});
  theirs.node_name = "media-node-2";
  theirs.traffic_window_ms = 10000;
  theirs.traffic.push_back({1, 100, 200, 10, 20});
  theirs.traffic.push_back({4, 300, 400, 30, 40});
  theirs.cache_hits = 5;
  theirs.cache_misses = 6;
  theirs.cache_evictions = 7;
  const auto macha_bytes = macha::encode_telemetry_set({theirs});

  const auto decoded = status::decode_telemetry_set(macha_bytes);
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(status::encode_telemetry_set(decoded), macha_bytes);
  EXPECT_EQ(decoded[0].sequence(), 42u);
  EXPECT_EQ(decoded[0].host(), "10.0.0.7");
  EXPECT_EQ(decoded[0].phase(), status::Phase::recovering);
  EXPECT_EQ(decoded[0].traffic().size(), 2u);

  // Dannan building the same record: its core fields, and Macha's own added
  // by the hosted system, put in protocol 23's order.
  status::Telemetry built;
  built.set_bytes(status::field::node_id, theirs.node_id.bytes);
  built.set_bytes(status::field::boot_id, theirs.boot_id.bytes);
  built.set_u64(status::field::sequence, 42);
  built.set_u64(status::field::observed_unix_ms, theirs.observed_unix_ms);
  built.set_text(status::field::version, "0.91.2");
  built.set_text(status::field::host, "10.0.0.7");
  built.set_text(status::field::failure_domain, "site-a");
  built.set_u16(status::field::port, 7437);
  built.set_u64(status::field::storage_capacity, theirs.storage_capacity);
  built.set_u64(status::field::storage_used, theirs.storage_used);
  built.set_u64(status::field::metadata_generation, 99);
  built.set_u64(status::field::uptime_ms, 5000);
  built.set_u64(status::field::rss_bytes, theirs.rss_bytes);
  built.set_u32(status::field::process_cpu_milli_percent, 1234);
  built.set_u32(status::field::load1_milli, 900);
  built.set_u32(status::field::peers_known, 3);
  built.set_u32(status::field::peers_active, 2);
  built.set_u64(status::field::connections_created, 7);
  built.set_u64(status::field::connections_reused, 70);
  built.set_u64(status::field::connections_canonical, 4);
  built.set_phase(status::Phase::recovering);
  built.set_u32(status::field::cpu_cores, 8);
  built.set_u64(status::field::memory_total_bytes, theirs.memory_total_bytes);
  built.set_text(status::field::node_name, "media-node-2");
  built.set_traffic(10000, {{1, 100, 200, 10, 20}, {4, 300, 400, 30, 40}});
  // What Macha adds as a hosted system, in any order: the layout sorts it.
  for (const auto& [id, value] : decoded[0].fields()) {
    if (!built.get(id)) built.set_bytes(id, value);
  }
  built.order_by(status::protocol23_layout());
  EXPECT_EQ(status::encode_telemetry_set({built}), macha_bytes);
  const auto back = macha::decode_telemetry_set(status::encode_telemetry_set({built}));
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(back[0].playback_transcode_rates.size(), 1u);
  EXPECT_EQ(back[0].cache_evictions, 7u);
}

// The observation log's lines are Macha's, byte for byte.
TEST(Interop, ObservationLinesAreMachas) {
  std::mt19937_64 random(7);
  status::ObservationSnapshot ours;
  macha::ObservationSnapshot theirs;
  for (const auto* name : {"rpc ping", "api GET /api/v1/status", "esc \"\\\n\x01", "empty"}) {
    status::HistogramSnapshot h;
    macha::HistogramSnapshot m;
    if (std::string(name) != "empty")
      for (int i = 0; i < 50; ++i) {
        const auto b = random() % status::bucket_count;
        h.buckets[b] += 1;
        m.buckets[b] += 1;
        h.count += 1;
        m.count += 1;
      }
    h.sum = m.sum = random() % 100000;
    h.max = m.max = random();
    ours.histograms[name] = h;
    theirs.histograms[name] = m;
  }
  ours.counters["a"] = theirs.counters["a"] = 3;
  ours.counters["zero"] = theirs.counters["zero"] = 0;
  const std::map<std::string, uint64_t> gauges{{"q", 9}, {"b", 1}};
  EXPECT_EQ(status::render_window(1, 2, "0.1.0", ours, gauges), macha::render_observation_window(1, 2, "0.1.0", theirs, gauges));
  const status::ObservationEvent event{5, "shutdown", {{"ms", 9}, {"n", 1}}, {{"why", "line\nbreak"}}};
  macha::ObservationEvent theirs_event;
  theirs_event.unix_ms = 5;
  theirs_event.name = "shutdown";
  theirs_event.numbers = event.numbers;
  theirs_event.texts = event.texts;
  EXPECT_EQ(status::render_event(event), macha::render_observation_event(theirs_event));
  for (const auto& [method, path] :
       std::vector<std::pair<std::string, std::string>>{{"GET", "/api/v1/a/B/c/d"}, {"PUT", "/x"}, {"TRACE", "/api/v2"}, {"DELETE", "/api/v1/sessions/0123"}})
    EXPECT_EQ(status::route_label(method, path), macha::observation_route_label(method, path));
}
}  // namespace
}  // namespace dannan
