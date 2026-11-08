//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/session.h"

#include <sys/socket.h>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <vector>

namespace dannan::net {
namespace asio = boost::asio;

namespace {

Bytes transfer_target(uint64_t request_id) {
  Writer writer;
  writer.u64(request_id);
  return writer.take();
}

std::optional<uint64_t> read_target(const Bytes& payload) {
  try {
    Reader reader(payload);
    const auto id = reader.u64();
    reader.finish();
    if (id) return id;
  } catch (const DecodeError&) {
  }
  return std::nullopt;
}

std::optional<sched::FrameType> promotion_of(MessageType type) {
  if (type == message::promote_foreground) return sched::FrameType::foreground;
  if (type == message::promote_read_ahead) return sched::FrameType::read_ahead;
  return std::nullopt;
}

}  // namespace

std::shared_ptr<Session> Session::create(Socket socket, Established established, Role role, std::shared_ptr<const RouteTable> routes, SessionHandlers handlers,
                                         SessionOptions options) {
  return std::shared_ptr<Session>(new Session(std::move(socket), std::move(established), role, std::move(routes), std::move(handlers), options));
}

Session::Session(Socket socket, Established established, Role role, std::shared_ptr<const RouteTable> routes, SessionHandlers handlers, SessionOptions options)
    : _socket(std::move(socket)),
      _strand(asio::make_strand(_socket.get_executor())),
      _established(std::move(established)),
      _role(role),
      _routes(std::move(routes)),
      _handlers(std::move(handlers)),
      _options(options),
      _opener(_established, _routes),
      _assembler(options.max_incomplete_messages, options.max_incomplete_bytes, options.max_message_bytes, options.memory),
      _sealer(_established, _routes),
      _queue(options.memory),
      _next_request(role == Role::dialler ? 1 : 2) {
#ifdef SO_NOSIGPIPE
  // A peer that goes away must never end the process: whatever socket the
  // session was given, a write to a closed one fails instead of signalling.
  const int on = 1;
  ::setsockopt(_socket.native_handle(), SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

Session::~Session() = default;

void Session::start() {
  asio::dispatch(_strand, [self = shared_from_this()] { self->read_header(); });
}

void Session::read_header() {
  asio::async_read(_socket, asio::buffer(_header), asio::bind_executor(_strand, [self = shared_from_this()](const boost::system::error_code& error, size_t) {
                     if (error) return self->fail(error == asio::error::eof ? "peer closed" : error.message());
                     // Read before the close landed: its owner may be gone.
                     if (self->is_closed()) return;
                     size_t rest = 0;
                     try {
                       rest = self->_opener.header(self->_header);
                     } catch (const std::exception& refused) {
                       return self->fail(refused.what());
                     }
                     // A byte of a reply is progress for the call awaiting it.
                     Reader peek(self->_header);
                     peek.view(20);
                     const auto request_id = peek.u64();
                     if (request_id && self->ours(request_id)) {
                       contract::Lock lock(self->_mutex);
                       if (const auto found = self->_pending.find(request_id); found != self->_pending.end())
                         found->second.last_progress = self->_options.time->now();
                     }
                     self->read_body(rest);
                   }));
}

void Session::read_body(size_t size) {
  _body.resize(size);
  asio::async_read(_socket, asio::buffer(_body),
                   asio::bind_executor(_strand, [self = shared_from_this(), size](const boost::system::error_code& error, size_t) {
                     if (error) return self->fail(error == asio::error::eof ? "peer closed" : error.message());
                     if (self->is_closed()) return;
                     try {
                       auto fragment = self->_opener.open(self->_body);
                       if (self->_options.traffic) self->_options.traffic->note(false, fragment.frame, frame_header_size + size);
                       if (auto frame = self->_assembler.push(std::move(fragment))) self->deliver(std::move(*frame));
                     } catch (const std::exception& refused) {
                       return self->fail(refused.what());
                     }
                     self->read_header();
                   }));
}

void Session::deliver(Frame frame) {
  frame.session_closed = _gone;
  if (frame.request_id == 0) {
    const auto type = frame.message.type;
    if (const auto promoted = promotion_of(type)) {
      if (const auto target = read_target(frame.message.payload)) {
        _assembler.promote(*target, *promoted);
        {
          contract::Lock lock(_mutex);
          _queue.promote_inbound(*target, *promoted);
        }
        if (_handlers.promoted) _handlers.promoted(*target, *promoted);
        kick_writer();
      }
    } else if (type == message::cancel_transfer) {
      if (const auto target = read_target(frame.message.payload)) {
        _assembler.discard(*target);
        {
          contract::Lock lock(_mutex);
          _queue.cancel_inbound(*target);
        }
        if (_handlers.cancelled) _handlers.cancelled(*target);
      }
    } else if (type == message::session_retire) {
      {
        contract::Lock lock(_mutex);
        _peer_retired = true;
        _retiring = true;
      }
      settle();
    } else if (_handlers.request && (_routes->is_notification(type) || !_routes->find(type))) {
      // A registered notification, or a user type nobody here registered:
      // the user's to take or ignore.
      try {
        _handlers.request(std::move(frame), Reply{});
      } catch (...) {
        // A notification has no one to answer; it is dropped.
      }
    }
    return;
  }
  if (ours(frame.request_id)) {
    Pending pending;
    {
      contract::Lock lock(_mutex);
      const auto found = _pending.find(frame.request_id);
      if (found == _pending.end()) return;  // cancelled, or a reply to nothing
      pending = std::move(found->second);
      _pending.erase(found);
    }
    asio::post(pending.executor, [done = std::move(pending.done), message = std::move(frame.message), peer = _established.peer]() mutable {
      done(CallOutcome{std::move(message), {}, std::move(peer)});
    });
    settle();
    return;
  }
  {
    contract::Lock lock(_mutex);
    _queue.register_inbound(frame.request_id, frame.frame);
  }
  const auto id = frame.request_id;
  const auto class_of = frame.frame;
  if (!_handlers.request) return queue_reply(id, class_of, error_reply(501, "not_implemented", "no handler on this session"));
  // A handler that throws answers its request; it never ends the session.
  try {
    _handlers.request(std::move(frame), Reply([weak = weak_from_this(), id, class_of](Message message) {
                        if (auto self = weak.lock()) self->queue_reply(id, class_of, std::move(message));
                      }));
  } catch (const std::exception& failure) {
    queue_reply(id, class_of, error_reply(500, "internal_error", failure.what()));
  } catch (...) {
    queue_reply(id, class_of, error_reply(500, "internal_error", "request handler failed"));
  }
}

void Session::queue_reply(uint64_t request_id, sched::FrameType frame, Message message) {
  bool overflow = false;
  {
    contract::Lock lock(_mutex);
    if (_closed) return;
    try {
      (void)_queue.push(request_id, frame, std::move(message), true);
    } catch (const OutboundRefused&) {
      overflow = true;
    }
  }
  // Past even the substitutes' bound: closing answers every caller.
  if (overflow) return close("peer outbound queue full of refusals");
  kick_writer();
}

void Session::queue_control(MessageType type, uint64_t target) {
  contract::Lock lock(_mutex);
  if (_closed) return;
  try {
    (void)_queue.push(0, sched::FrameType::control, Message{type, transfer_target(target), {}}, false);
  } catch (const OutboundRefused&) {
    // A lost transfer control costs the peer some wasted work, nothing more.
  }
}

Admission Session::call(MessageType type, Bytes&& payload, sched::FrameType frame, asio::any_io_executor executor, std::function<void(CallOutcome)> done) {
  Admission out;
  {
    contract::Lock lock(_mutex);
    if (_closed) return {0, "peer channel is closed"};
    if (_retiring) return {0, "peer channel is retiring"};
    if (_pending.size() >= max_pending_replies) return {0, "peer pending reply limit reached"};
    if (!_routes->allowed(type, _established.lane, frame))
      return {0,
              "message " + _routes->name(type) + " may not travel as " + sched::frame_type_name(frame) + " on the " + lane_name(_established.lane) + " lane"};
    const auto id = _next_request;
    try {
      (void)_queue.push(id, frame, Message{type, std::move(payload), {}}, false);
    } catch (const OutboundRefused& refused) {
      return {0, refused.what()};
    }
    _next_request += 2;
    _pending.emplace(id, Pending{std::move(done), std::move(executor), _options.time->now()});
    out.request_id = id;
  }
  kick_writer();
  return out;
}

void Session::notify(Message message, sched::FrameType frame, asio::any_io_executor executor, std::function<void(bool)> sent) {
  {
    contract::Lock lock(_mutex);
    if (_closed) {
      if (sent) asio::post(executor, [sent] { sent(false); });
      return;
    }
    OutboundQueue::Sent told;
    if (sent) told = [executor, sent](bool ok) { asio::post(executor, [sent, ok] { sent(ok); }); };
    (void)_queue.push(0, frame, std::move(message), false, std::move(told));
  }
  kick_writer();
}

bool Session::try_notify(Message message, sched::FrameType frame) {
  {
    contract::Lock lock(_mutex);
    if (_closed || _retiring || !_routes->allowed(message.type, _established.lane, frame)) return false;
    const bool small = message.payload.size() <= max_try_notify_payload;
    if (_queue.size() && (!small || _queue.bytes() > max_try_notify_backlog)) return false;
    try {
      (void)_queue.push(0, frame, std::move(message), false);
    } catch (const OutboundRefused&) {
      return false;
    }
  }
  kick_writer();
  return true;
}

void Session::note_path_progress() { _path_progress.store(_options.time->now()); }

void Session::cancel(uint64_t request_id) {
  std::optional<Pending> pending;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _pending.find(request_id); found != _pending.end()) {
      pending = std::move(found->second);
      _pending.erase(found);
    }
    _queue.cancel_outgoing(request_id);
  }
  if (pending) asio::post(pending->executor, [done = std::move(pending->done)] { done(CallOutcome{std::nullopt, "RPC cancelled", {}}); });
  queue_control(message::cancel_transfer, request_id);
  settle();
}

void Session::promote(uint64_t request_id, sched::FrameType frame) {
  const auto type = frame == sched::FrameType::foreground   ? message::promote_foreground
                    : frame == sched::FrameType::read_ahead ? message::promote_read_ahead
                                                            : MessageType{};
  if (type == MessageType{}) return;
  {
    contract::Lock lock(_mutex);
    _queue.promote_outgoing(request_id, frame);
  }
  queue_control(type, request_id);
  kick_writer();
}

void Session::kick_writer() {
  asio::post(_strand, [self = shared_from_this()] { self->write_next(); });
}

void Session::write_next() {
  std::optional<OutboundQueue::Fragment> fragment;
  {
    contract::Lock lock(_mutex);
    if (_closed || _writing) return;
    fragment = _queue.next(_established.max_frame);
    if (!fragment) return;
    _writing = true;
  }
  // The fragment's payload stays put until written(); only this strand
  // touches the sealer.
  try {
    _wire = _sealer.seal(fragment->request_id, fragment->frame, fragment->type, fragment->first, fragment->last, fragment->payload);
  } catch (const std::exception& error) {
    return fail(error.what(), true);
  }
  const auto frame = fragment->frame;
  const auto request_id = fragment->request_id;
  asio::async_write(_socket, asio::buffer(_wire),
                    asio::bind_executor(_strand, [self = shared_from_this(), frame, request_id](const boost::system::error_code& error, size_t bytes) {
                      if (error) return self->fail(error.message(), true);
                      if (self->_options.traffic) self->_options.traffic->note(true, frame, bytes);
                      {
                        contract::Lock lock(self->_mutex);
                        self->_writing = false;
                        self->_queue.written();
                        // A call whose bytes are leaving is making progress, as one whose
                        // reply is arriving is: an upload is not idle for having no answer yet.
                        if (request_id && self->ours(request_id)) {
                          if (const auto found = self->_pending.find(request_id); found != self->_pending.end())
                            found->second.last_progress = self->_options.time->now();
                        }
                      }
                      self->settle();
                    }));
}

// Closed at once, so nothing is admitted after close() returns; the socket
// and the owner's handler are dealt with on the strand.
void Session::close(const std::string& why) {
  if (!mark_closed(why)) return;
  asio::dispatch(_strand, [self = shared_from_this(), why] { self->shut(why); });
}

void Session::fail(const std::string& why, bool writing) {
  if (!mark_closed(why)) return;
  if (_options.log) {
    const std::string said = _role == Role::dialler ? (writing ? "peer writer: " : "peer channel: ") : (writing ? "accepted peer writer: " : "RPC session: ");
    status::ScopedLogger(_options.log, "net").debug(said + why);
  }
  shut(why);
}

bool Session::mark_closed(const std::string& why) {
  std::map<uint64_t, Pending> pending;
  {
    contract::Lock lock(_mutex);
    if (_closed) return false;
    _closed = true;
    _gone->store(true);
    pending.swap(_pending);
    // The queue is left to shut(), on the strand: the writer may be sealing
    // a fragment of it outside the lock.
  }
  for (auto& [_, call] : pending)
    asio::post(call.executor, [done = std::move(call.done), why] { done(CallOutcome{std::nullopt, "peer channel closed: " + why, {}}); });
  return true;
}

void Session::shut(const std::string& why) {
  {
    contract::Lock lock(_mutex);
    _queue.abandon();
  }
  // Nothing it holds reaches its owner from here: the owner may be stopping, and the
  // ledger the partial messages are charged to is the owner's.
  _assembler.clear();
  _handlers.request = {};
  _handlers.promoted = {};
  _handlers.cancelled = {};
  boost::system::error_code ignored;
  _socket.shutdown(Socket::shutdown_both, ignored);
  _socket.close(ignored);
  if (_handlers.closed) {
    auto told = std::move(_handlers.closed);
    _handlers.closed = {};
    told(why);
  }
}

bool Session::is_closed() const {
  contract::Lock lock(_mutex);
  return _closed;
}

void Session::after_queued(std::function<void()> done) { asio::post(_strand, std::move(done)); }

bool Session::usable() const {
  contract::Lock lock(_mutex);
  return !_closed && !_retiring;
}

void Session::retire() {
  {
    contract::Lock lock(_mutex);
    if (_closed || _retiring) return;
    _retiring = true;
  }
  settle();
}

void Session::settle() {
  bool finished = false;
  {
    contract::Lock lock(_mutex);
    if (_closed) return;
    if (_retiring && !_retire_sent && _queue.size() == 0 && !_writing) {
      _retire_sent = true;
      try {
        (void)_queue.push(0, sched::FrameType::control, Message{message::session_retire, {}, {}}, false);
      } catch (const OutboundRefused&) {
        _retire_sent = false;
      }
    }
    finished = _retiring && _retire_sent && _peer_retired && _pending.empty() && _queue.drained() && !_writing;
  }
  if (finished) return close("retired");
  kick_writer();
}

size_t Session::pending() const {
  contract::Lock lock(_mutex);
  return _pending.size();
}

std::optional<contract::Clock::duration> Session::idle_for(uint64_t request_id) const {
  contract::Lock lock(_mutex);
  const auto found = _pending.find(request_id);
  if (found == _pending.end()) return std::nullopt;
  return _options.time->now() - found->second.last_progress;
}

namespace {

struct DialState {
  Session::Socket& socket;
  std::unique_ptr<ClientHandshake> client;
  std::array<uint8_t, 4> length{};
  Bytes body;
  HandshakeDone done;
};

struct AcceptState {
  Session::Socket& socket;
  Credentials credentials;
  membership::NodeRecord local;
  size_t max_frame;
  Endpoint remote;
  std::optional<Expected> expected;
  std::array<uint8_t, 4> length{};
  Bytes body;
  Bytes reply;
  Established session;
  HandshakeDone done;
};

uint32_t blob_length(const std::array<uint8_t, 4>& bytes) {
  Reader reader(bytes);
  return reader.u32();
}

template <class State>
void refuse(const std::shared_ptr<State>& state, const std::string& why) {
  state->done(std::make_exception_ptr(HandshakeRefused(why)), {});
}

}  // namespace

void async_dial_handshake(Session::Socket& socket, const Credentials& credentials, membership::NodeRecord local, size_t max_frame, Lane lane,
                          HandshakeDone done, Purpose purpose, PathToken token) {
  std::shared_ptr<DialState> state;
  try {
    state = std::make_shared<DialState>(
        DialState{socket, std::make_unique<ClientHandshake>(credentials, std::move(local), max_frame, lane, purpose, token), {}, {}, std::move(done)});
  } catch (...) {
    return done(std::current_exception(), {});
  }
  asio::async_write(socket, asio::buffer(state->client->hello_blob()), [state](const boost::system::error_code& error, size_t) {
    if (error) return refuse(state, "handshake write: " + error.message());
    asio::async_read(state->socket, asio::buffer(state->length), [state](const boost::system::error_code& error, size_t) {
      if (error) return refuse(state, "handshake read: " + error.message());
      const auto size = blob_length(state->length);
      if (size > max_handshake_blob) return refuse(state, "blob too large");
      state->body.resize(size);
      asio::async_read(state->socket, asio::buffer(state->body), [state](const boost::system::error_code& error, size_t) {
        if (error) return refuse(state, "handshake read: " + error.message());
        Established established;
        try {
          established = state->client->finish(state->body);
        } catch (...) {
          return state->done(std::current_exception(), {});
        }
        state->done(nullptr, std::move(established));
      });
    });
  });
}

void async_accept_handshake(Session::Socket& socket, const Credentials& credentials, membership::NodeRecord local, size_t max_frame, Endpoint remote,
                            HandshakeDone done, std::optional<Expected> expected) {
  auto state = std::make_shared<AcceptState>(
      AcceptState{socket, credentials, std::move(local), max_frame, std::move(remote), std::move(expected), {}, {}, {}, {}, std::move(done)});
  asio::async_read(socket, asio::buffer(state->length), [state](const boost::system::error_code& error, size_t) {
    if (error) return refuse(state, "handshake read: " + error.message());
    const auto size = blob_length(state->length);
    if (size > max_handshake_blob) return refuse(state, "blob too large");
    state->body.resize(size);
    asio::async_read(state->socket, asio::buffer(state->body), [state](const boost::system::error_code& error, size_t) {
      if (error) return refuse(state, "handshake read: " + error.message());
      try {
        auto reply =
            server_handshake(state->credentials, state->local, state->max_frame, state->body, state->remote, state->expected ? &*state->expected : nullptr);
        state->reply = std::move(reply.response_blob);
        state->session = std::move(reply.session);
      } catch (...) {
        return state->done(std::current_exception(), {});
      }
      asio::async_write(state->socket, asio::buffer(state->reply), [state](const boost::system::error_code& error, size_t) {
        if (error) return refuse(state, "handshake write: " + error.message());
        state->done(nullptr, std::move(state->session));
      });
    });
  });
}

}  // namespace dannan::net
