//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The first bytes a stranger sends: a hello to the server, an ack to the
// client, and a frame's header and body. Each may throw only its refusal
// (HandshakeRefused, FrameRefused).

#include "dannan/fuzz/fuzz.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/net/channel.h"

namespace dannan::fuzz {
namespace {

// A fixed key, so seeds made with it authenticate and a mutator gets past
// the MAC to the parser behind it.
const identity::ClusterKey& key() {
  static const identity::ClusterKey value = [] {
    identity::ClusterKey out;
    out.master.fill(7);
    out.auth.fill(8);
    out.cluster_id.fill(9);
    return out;
  }();
  return value;
}

membership::NodeRecord node(uint8_t n) {
  membership::NodeRecord out;
  out.id.bytes.fill(n);
  out.host = "10.0.0." + std::to_string(n);
  out.port = 7437;
  return out;
}

std::span<const uint8_t> body(const net::Bytes& blob) { return std::span(blob).subspan(4); }

const Registration hello(
    "handshake_hello",
    [](std::span<const uint8_t> data) {
      try {
        (void)net::server_handshake(key(), node(2), 65536, data, {"192.0.2.1", 1});
      } catch (const net::HandshakeRefused&) {
      }
    },
    [] {
      net::ClientHandshake client(key(), node(1), 65536, net::Lane::control);
      const auto& blob = client.hello_blob();
      return std::vector<net::Bytes>{net::Bytes(body(blob).begin(), body(blob).end())};
    });

const Registration ack(
    "handshake_ack",
    [](std::span<const uint8_t> data) {
      net::ClientHandshake client(key(), node(1), 65536, net::Lane::control);
      try {
        (void)client.finish(data);
      } catch (const net::HandshakeRefused&) {
      }
    },
    [] {
      // An ack to another hello authenticates under no client, but its shape
      // is right, which is what the mutator needs.
      net::ClientHandshake client(key(), node(1), 65536, net::Lane::control);
      const auto reply = net::server_handshake(key(), node(2), 65536, body(client.hello_blob()), {"192.0.2.1", 1});
      return std::vector<net::Bytes>{net::Bytes(body(reply.response_blob).begin(), body(reply.response_blob).end())};
    });

const Registration frame(
    "frame",
    [](std::span<const uint8_t> data) {
      static const auto pair = [] {
        net::ClientHandshake client(key(), node(1), 65536, net::Lane::control);
        auto reply = net::server_handshake(key(), node(2), 65536, body(client.hello_blob()), {"192.0.2.1", 1});
        return std::make_pair(client.finish(body(reply.response_blob)), reply.session);
      }();
      const auto routes = std::make_shared<const net::RouteTable>(net::RouteTable::with_dannan_types());
      net::FrameOpener opener(pair.second, routes);
      try {
        if (data.size() < net::frame_header_size) return;
        const auto rest = opener.header(std::span<const uint8_t, net::frame_header_size>(data.data(), net::frame_header_size));
        (void)rest;
        (void)opener.open(data.subspan(net::frame_header_size));
      } catch (const net::FrameRefused&) {
      }
    },
    [] {
      net::ClientHandshake client(key(), node(1), 65536, net::Lane::control);
      auto reply = net::server_handshake(key(), node(2), 65536, body(client.hello_blob()), {"192.0.2.1", 1});
      const auto established = client.finish(body(reply.response_blob));
      const auto routes = std::make_shared<const net::RouteTable>(net::RouteTable::with_dannan_types());
      net::FrameSealer sealer(established, routes);
      return std::vector<net::Bytes>{sealer.seal(1, sched::FrameType::control, net::message::members, true, true, net::Bytes{1, 2, 3})};
    });

}  // namespace
}  // namespace dannan::fuzz
