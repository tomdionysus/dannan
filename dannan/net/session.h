//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/asio/strand.hpp>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/net/assembler.h"
#include "dannan/net/channel.h"
#include "dannan/net/message.h"
#include "dannan/net/outbound.h"

namespace dannan::net {

// The node's cluster traffic in and out by frame class: every sealed
// fragment with its overhead, handshakes not counted. One instance per node,
// shared by every session it has, dialled or accepted.
struct TransportTraffic {
  static constexpr size_t classes = 6;  // indexed by the frame type's wire value
  std::array<std::atomic<uint64_t>, classes> in_bytes{};
  std::array<std::atomic<uint64_t>, classes> out_bytes{};
  void note(bool outbound, sched::FrameType frame, uint64_t bytes) noexcept {
    const auto index = static_cast<size_t>(frame);
    if (index < classes) (outbound ? out_bytes : in_bytes)[index].fetch_add(bytes, std::memory_order_relaxed);
  }
};

// Which end of the connection this session is. The dialler numbers its
// requests odd, the acceptor even, so a request id says who asked.
enum class Role : uint8_t { dialler, acceptor };

// How a call ended: the peer's reply (which may be an error carrying a
// refusal), or, when no reply came, why not.
struct CallOutcome {
  std::optional<Message> reply;
  std::string failure;
};

// Whether a call was admitted: its request id, or why not ("peer pending
// reply limit reached", "peer outbound queue full", "peer channel is
// closed"), so a caller can tell a busy route from a dead one.
struct Admission {
  uint64_t request_id = 0;
  std::string refused;
};

// The way to answer one inbound request; empty for a notification. Callable
// once, from any thread; a session's reply goes at the request's class and
// is never dropped for capacity, and after the session closes it goes
// nowhere. Later calls do nothing.
class Reply {
 public:
  Reply() = default;
  explicit Reply(std::function<void(Message)> send) : _send(std::move(send)) {}
  explicit operator bool() const noexcept { return static_cast<bool>(_send); }
  void operator()(Message message) {
    if (auto send = std::exchange(_send, {})) send(std::move(message));
  }

 private:
  std::function<void(Message)> _send;
};

// What a session's owner is told. Called on the session's strand, in
// arrival order; a handler must not block (the server queues the work).
struct SessionHandlers {
  // A peer's request, or a registered notification (with an empty Reply).
  std::function<void(Frame, Reply)> request;
  // The peer promoted, or cancelled, one of its requests.
  std::function<void(uint64_t, sched::FrameType)> promoted;
  std::function<void(uint64_t)> cancelled;
  // The session ended, and why. Called once.
  std::function<void(std::string)> closed;
};

struct SessionOptions {
  sched::RetainedMemoryLedger* memory = nullptr;
  std::shared_ptr<TransportTraffic> traffic;
  const contract::TimeSource* time = &contract::steady_time_source();
  size_t max_incomplete_messages = 64;
  size_t max_incomplete_bytes = 256ull * 1024 * 1024;
  size_t max_message_bytes = 128ull * 1024 * 1024;
};

// One authenticated connection to a peer, on one lane, on Asio: a reader and
// a writer on the session's strand, the outbound queue choosing frame by
// frame, calls with their replies, notifications and transfer controls. All
// public members are thread-safe; every completion is posted to the
// executor the caller passed, never run inline.
class Session : public std::enable_shared_from_this<Session> {
 public:
  using Socket = boost::asio::generic::stream_protocol::socket;

  static std::shared_ptr<Session> create(Socket socket, Established established, Role role, std::shared_ptr<const RouteTable> routes, SessionHandlers handlers,
                                         SessionOptions options = {});
  ~Session();

  // Starts reading and writing.
  void start();

  // Places a call. Admitted, its completion is posted to `executor` exactly
  // once: the reply, or why none came (closed, cancelled). Refused, nothing
  // is posted and the admission says why. Waits on nothing.
  Admission call(MessageType type, Bytes payload, sched::FrameType frame, boost::asio::any_io_executor executor, std::function<void(CallOutcome)> done);
  // Sends a notification; `sent` (optional) is posted to `executor` with
  // whether it was written. Throws OutboundRefused when the queue will not
  // take it.
  void notify(Message message, sched::FrameType frame, boost::asio::any_io_executor executor, std::function<void(bool)> sent = {});
  // Cancels a call: it completes at once as cancelled, the peer is told,
  // and a late reply is dropped.
  void cancel(uint64_t request_id);
  // Promotes a call to foreground or read-ahead, here and at the peer.
  void promote(uint64_t request_id, sched::FrameType frame);
  // Ends the session: every waiting call fails, the owner is told why.
  void close(const std::string& why);

  bool usable() const;
  size_t pending() const;
  // How long a call has gone without a byte of its reply, by the session's
  // time source; empty for a call not pending.
  std::optional<contract::Clock::duration> idle_for(uint64_t request_id) const;
  const Established& established() const noexcept { return _established; }
  const membership::NodeRecord& peer() const noexcept { return _established.peer; }
  Lane lane() const noexcept { return _established.lane; }

 private:
  struct Pending {
    std::function<void(CallOutcome)> done;
    boost::asio::any_io_executor executor;
    contract::TimePoint last_progress;
  };

  Session(Socket socket, Established established, Role role, std::shared_ptr<const RouteTable> routes, SessionHandlers handlers, SessionOptions options);

  bool ours(uint64_t request_id) const noexcept { return (request_id % 2 == 1) == (_role == Role::dialler); }
  void read_header();
  void read_body(size_t size);
  void deliver(Frame frame);
  void queue_reply(uint64_t request_id, sched::FrameType frame, Message message);
  void queue_control(MessageType type, uint64_t target);
  void kick_writer();
  void write_next();
  // Called on the strand when the connection breaks.
  void fail(const std::string& why);
  // Marks the session closed and fails every waiting call; false if it
  // already was.
  bool mark_closed(const std::string& why);
  // On the strand: ends the socket and tells the owner.
  void shut(const std::string& why);

  Socket _socket;
  boost::asio::strand<boost::asio::any_io_executor> _strand;
  const Established _established;
  const Role _role;
  const std::shared_ptr<const RouteTable> _routes;
  SessionHandlers _handlers;
  const SessionOptions _options;
  // Reader state, on the strand.
  FrameOpener _opener;
  MessageAssembler _assembler;
  std::array<uint8_t, frame_header_size> _header{};
  Bytes _body;
  // Writer state, on the strand.
  FrameSealer _sealer;
  Bytes _wire;
  // Memory work only: the queue, the calls waiting on replies, the ids.
  mutable contract::Mutex _mutex;
  OutboundQueue _queue DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, Pending> _pending DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_request DANNAN_GUARDED_BY(_mutex);
  bool _writing DANNAN_GUARDED_BY(_mutex) = false;
  bool _closed DANNAN_GUARDED_BY(_mutex) = false;
};

// The client's and the server's handshakes on a connected socket. The
// handler is called once, on an io thread, with the session established or
// the reason it was not; the caller bounds the wait (a timer that closes the
// socket).
using HandshakeDone = std::function<void(std::exception_ptr, Established)>;
void async_dial_handshake(Session::Socket& socket, const identity::ClusterKey& key, membership::NodeRecord local, size_t max_frame, Lane lane,
                          HandshakeDone done);
void async_accept_handshake(Session::Socket& socket, const identity::ClusterKey& key, membership::NodeRecord local, size_t max_frame, std::string remote_host,
                            HandshakeDone done);

}  // namespace dannan::net
