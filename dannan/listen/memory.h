//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "dannan/listen/carrier.h"

// An in-memory pipe as two carriers (TODO/2026-11-20-stack-plan.md, item 17): a connection with
// no socket, for a stream inside the simulation's world or a test. It behaves as a TCP connection
// does: a bounded buffer each way, a write that waits while the reader is behind, the end of the
// stream after a half-close, a reset after a sever. Each end delivers on its own executor (the
// world's io_context in a simulation), so a seed replays exactly. The test's hand is the
// MemoryLink: bytes in pieces it chooses, a stall, a resume, a sever. Thread-safe.
namespace dannan::listen {

enum class End : uint8_t { client, server };

struct MemoryOptions {
  // What each end reports as the other's address (Lugh: rate limits, back-off and
  // X-Forwarded-For key on it).
  boost::asio::ip::tcp::endpoint client_address{boost::asio::ip::make_address("127.0.0.1"), 40000};
  boost::asio::ip::tcp::endpoint server_address{boost::asio::ip::make_address("127.0.0.1"), 7000};
  // Bytes buffered each way before a write waits for the reader.
  size_t capacity = 256 * 1024;
};

class MemoryLink {
 public:
  virtual ~MemoryLink() = default;
  // Both ends see the connection reset: what is waiting ends with connection_reset, and so does
  // every read and write after. A dropped TCP connection.
  virtual void sever() = 0;
  // No byte is delivered either way and nothing closes: a half-open connection, which a
  // protocol's keep-alive must notice. Writes are taken while the buffer has room.
  virtual void stall() = 0;
  // Delivery again, in the order written.
  virtual void resume() = 0;
  // The most bytes one read takes of what end `from` wrote, given how many are waiting: a packet
  // reader tested across splits the test chooses. Unset, a read takes all it can.
  virtual void set_pieces(End from, std::function<size_t(size_t waiting)> piece) = 0;
  virtual bool severed() const = 0;
  virtual bool stalled() const = 0;
};

struct MemoryPair {
  std::unique_ptr<Carrier> client;
  std::unique_ptr<Carrier> server;
  std::shared_ptr<MemoryLink> link;
};

// A linked pair: the client end delivering on `client`, the server end on `server`.
MemoryPair memory_pair(boost::asio::any_io_executor client, boost::asio::any_io_executor server, MemoryOptions options = {});

}  // namespace dannan::listen
