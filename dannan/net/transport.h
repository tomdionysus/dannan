//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "dannan/net/client.h"
#include "dannan/net/server.h"

// What a node asks of the network, at the level of messages to peers (the simulation harness's
// transport seam, step 3 part 4, reviewed by Mimir and Kvasir: `2026-11-17-sim-transport-seam.md`).
// The real one is the client and the server over sockets; the harness gives its own, which
// delivers each message as an event of its world. Lanes and frame classes travel as values.
// Relay, punching, dial-back and probes stay with the real client: a simulated node is
// reachable or not by its world.
namespace dannan::net {

// What a node gives a transport of the harness's own (NodeConfig::transport).
struct TransportHooks {
  // The node's own record, read at each message it sends.
  std::function<membership::NodeRecord()> local;
  // A peer's request or notification, served as the server would serve it: answered by
  // returning, or deferred through the Answer.
  DeferringHandler serve;
  // A peer heard from directly, its record as it sent it: what a handshake tells a real node.
  std::function<void(const membership::NodeRecord&)> heard;
};

class Transport {
 public:
  virtual ~Transport() = default;

  // Binds and begins serving; then begins calling (health pings and maintained lanes). Throws
  // if the address cannot be bound.
  virtual void listen() = 0;
  virtual void start() = 0;
  virtual uint16_t bound_port() const = 0;
  // Stops serving: closes every session, waits for handlers. Must not run on the io context's
  // only thread.
  virtual void stop_serving() = 0;
  // Fails every pending call at once and admits nothing more.
  virtual void stop_calling() = 0;

  // As Client's: `done` posted to `executor` exactly once; waits on nothing.
  virtual std::shared_ptr<CallHandle> call(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame,
                                           boost::asio::any_io_executor executor, Client::Done done, CallWatch watch = {}) = 0;
  virtual std::shared_ptr<CallHandle> call(const Endpoint& endpoint, MessageType type, Bytes payload, sched::FrameType frame,
                                           boost::asio::any_io_executor executor, Client::Done done, CallWatch watch = {}) = 0;
  virtual bool send(const identity::NodeId& peer, Lane lane, const Message& message, sched::FrameType frame, bool best_effort = false) = 0;
  virtual size_t broadcast(const Message& message) = 0;
  virtual size_t broadcast_best_effort(const Message& message, sched::FrameType frame) = 0;
  virtual size_t broadcast_notify(const Message& message, sched::FrameType frame) = 0;

  virtual bool has_route(const identity::NodeId& peer, Lane lane) const = 0;
  virtual void close_route(const identity::NodeId& peer, Lane lane, const std::string& why) = 0;
  virtual void note_peer(const membership::NodeRecord& peer) = 0;
  virtual void set_maintained_peers(std::function<std::vector<membership::NodeRecord>()> peers) = 0;
  virtual void invalidate_identity_association(const membership::IdentityReset& reset) = 0;

  // Figures.
  virtual ClientStats stats() const = 0;
  virtual WorkStats work_stats() const = 0;
  virtual std::optional<std::chrono::milliseconds> peer_latency(const identity::NodeId& peer) const = 0;
  virtual std::map<identity::NodeId, std::chrono::milliseconds> peer_latencies() const = 0;
  virtual std::optional<Purpose> path_of(const identity::NodeId& peer, Lane lane) const = 0;
  // Tests' stalls (ClientOptions::call_fault).
  virtual size_t release_stalled(const std::optional<identity::NodeId>& peer) = 0;
  virtual size_t stalled() const = 0;
};

// The client and the server over sockets, as the node has always had them: every call
// forwarded, nothing added.
class RealTransport final : public Transport {
 public:
  RealTransport(Client& client, Server& server) noexcept : _client(client), _server(server) {}

  void listen() override { _server.start(); }
  void start() override { _client.start(); }
  uint16_t bound_port() const override { return _server.bound_port(); }
  void stop_serving() override { _server.stop(); }
  void stop_calling() override { _client.stop(); }

  std::shared_ptr<CallHandle> call(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame,
                                   boost::asio::any_io_executor executor, Client::Done done, CallWatch watch = {}) override {
    return _client.call(peer, type, std::move(payload), frame, std::move(executor), std::move(done), watch);
  }
  std::shared_ptr<CallHandle> call(const Endpoint& endpoint, MessageType type, Bytes payload, sched::FrameType frame, boost::asio::any_io_executor executor,
                                   Client::Done done, CallWatch watch = {}) override {
    return _client.call(endpoint, type, std::move(payload), frame, std::move(executor), std::move(done), watch);
  }
  bool send(const identity::NodeId& peer, Lane lane, const Message& message, sched::FrameType frame, bool best_effort = false) override {
    return _client.send(peer, lane, message, frame, best_effort);
  }
  size_t broadcast(const Message& message) override { return _client.broadcast(message); }
  size_t broadcast_best_effort(const Message& message, sched::FrameType frame) override { return _client.broadcast_best_effort(message, frame); }
  size_t broadcast_notify(const Message& message, sched::FrameType frame) override { return _client.broadcast_notify(message, frame); }

  bool has_route(const identity::NodeId& peer, Lane lane) const override { return _client.has_route(peer, lane); }
  void close_route(const identity::NodeId& peer, Lane lane, const std::string& why) override { _client.close_route(peer, lane, why); }
  void note_peer(const membership::NodeRecord& peer) override { _client.note_peer(peer); }
  void set_maintained_peers(std::function<std::vector<membership::NodeRecord>()> peers) override { _client.set_maintained_peers(std::move(peers)); }
  void invalidate_identity_association(const membership::IdentityReset& reset) override { _client.invalidate_identity_association(reset); }

  ClientStats stats() const override { return _client.stats(); }
  WorkStats work_stats() const override { return _server.work_stats(); }
  std::optional<std::chrono::milliseconds> peer_latency(const identity::NodeId& peer) const override { return _client.peer_latency(peer); }
  std::map<identity::NodeId, std::chrono::milliseconds> peer_latencies() const override { return _client.peer_latencies(); }
  std::optional<Purpose> path_of(const identity::NodeId& peer, Lane lane) const override { return _client.path_of(peer, lane); }
  size_t release_stalled(const std::optional<identity::NodeId>& peer) override { return _client.release_stalled(peer); }
  size_t stalled() const override { return _client.stalled(); }

 private:
  Client& _client;
  Server& _server;
};

}  // namespace dannan::net
