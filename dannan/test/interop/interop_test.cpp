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

#include <fstream>
#include <thread>

#include "cluster/net.hpp"
#include "codec.hpp"
#include "crypto.hpp"
#include "dannan/identity/cluster_key.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/channel.h"
#include "dannan/test/fixtures/temp_dir.h"
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

}  // namespace
}  // namespace dannan
