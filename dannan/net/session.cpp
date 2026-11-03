//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/session.h"

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

void Reply::operator()(Message message) {
  if (!_request_id) return;
  const auto id = std::exchange(_request_id, 0);
  if (auto session = _session.lock()) session->queue_reply(id, _frame, std::move(message));
}

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
      _next_request(role == Role::dialler ? 1 : 2) {}

Session::~Session() = default;

void Session::start() {
  asio::dispatch(_strand, [self = shared_from_this()] { self->read_header(); });
}

void Session::read_header() {
  asio::async_read(_socket, asio::buffer(_header), asio::bind_executor(_strand, [self = shared_from_this()](const boost::system::error_code& error, size_t) {
                     if (error) return self->fail(error == asio::error::eof ? "peer closed" : error.message());
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
      // Retirement drains, then closes; the connection owner decides when.
    } else if (_routes->is_notification(type) && _handlers.request) {
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
    asio::post(pending.executor, [done = std::move(pending.done), message = std::move(frame.message)]() mutable { done(CallOutcome{std::move(message), {}}); });
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
    _handlers.request(std::move(frame), Reply(weak_from_this(), id, class_of));
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

Admission Session::call(MessageType type, Bytes payload, sched::FrameType frame, asio::any_io_executor executor, std::function<void(CallOutcome)> done) {
  Admission out;
  {
    contract::Lock lock(_mutex);
    if (_closed) return {0, "peer channel is closed"};
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
  if (pending) asio::post(pending->executor, [done = std::move(pending->done)] { done(CallOutcome{std::nullopt, "RPC cancelled"}); });
  queue_control(message::cancel_transfer, request_id);
  kick_writer();
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
    return fail(error.what());
  }
  const auto frame = fragment->frame;
  asio::async_write(_socket, asio::buffer(_wire),
                    asio::bind_executor(_strand, [self = shared_from_this(), frame](const boost::system::error_code& error, size_t bytes) {
                      if (error) return self->fail(error.message());
                      if (self->_options.traffic) self->_options.traffic->note(true, frame, bytes);
                      {
                        contract::Lock lock(self->_mutex);
                        self->_writing = false;
                        self->_queue.written();
                      }
                      self->write_next();
                    }));
}

// Closed at once, so nothing is admitted after close() returns; the socket
// and the owner's handler are dealt with on the strand.
void Session::close(const std::string& why) {
  if (!mark_closed(why)) return;
  asio::dispatch(_strand, [self = shared_from_this(), why] { self->shut(why); });
}

void Session::fail(const std::string& why) {
  if (mark_closed(why)) shut(why);
}

bool Session::mark_closed(const std::string& why) {
  std::map<uint64_t, Pending> pending;
  {
    contract::Lock lock(_mutex);
    if (_closed) return false;
    _closed = true;
    pending.swap(_pending);
    _queue.abandon();
  }
  for (auto& [_, call] : pending)
    asio::post(call.executor, [done = std::move(call.done), why] { done(CallOutcome{std::nullopt, "peer channel closed: " + why}); });
  return true;
}

void Session::shut(const std::string& why) {
  boost::system::error_code ignored;
  _socket.shutdown(Socket::shutdown_both, ignored);
  _socket.close(ignored);
  if (_handlers.closed) {
    auto told = std::move(_handlers.closed);
    _handlers.closed = {};
    told(why);
  }
}

bool Session::usable() const {
  contract::Lock lock(_mutex);
  return !_closed;
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
  identity::ClusterKey key;
  membership::NodeRecord local;
  size_t max_frame;
  std::string remote_host;
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

void async_dial_handshake(Session::Socket& socket, const identity::ClusterKey& key, membership::NodeRecord local, size_t max_frame, Lane lane,
                          HandshakeDone done) {
  std::shared_ptr<DialState> state;
  try {
    state = std::make_shared<DialState>(DialState{socket, std::make_unique<ClientHandshake>(key, std::move(local), max_frame, lane), {}, {}, std::move(done)});
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

void async_accept_handshake(Session::Socket& socket, const identity::ClusterKey& key, membership::NodeRecord local, size_t max_frame, std::string remote_host,
                            HandshakeDone done) {
  auto state = std::make_shared<AcceptState>(AcceptState{socket, key, std::move(local), max_frame, std::move(remote_host), {}, {}, {}, {}, std::move(done)});
  asio::async_read(socket, asio::buffer(state->length), [state](const boost::system::error_code& error, size_t) {
    if (error) return refuse(state, "handshake read: " + error.message());
    const auto size = blob_length(state->length);
    if (size > max_handshake_blob) return refuse(state, "blob too large");
    state->body.resize(size);
    asio::async_read(state->socket, asio::buffer(state->body), [state](const boost::system::error_code& error, size_t) {
      if (error) return refuse(state, "handshake read: " + error.message());
      try {
        auto reply = server_handshake(state->key, state->local, state->max_frame, state->body, state->remote_host);
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
