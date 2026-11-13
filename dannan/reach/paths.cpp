//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/reach/paths.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

#include "dannan/identity/crypto.h"
#include "dannan/net/server.h"
#include "dannan/supervise/supervised.h"

namespace dannan::reach {

namespace asio = boost::asio;
using membership::NodeRecord;
namespace m = net::message;

namespace {

void put(net::Writer& writer, const net::Endpoint& endpoint) {
  writer.string(endpoint.host);
  writer.u16(endpoint.port);
}

net::Endpoint get_endpoint(net::Reader& reader) {
  net::Endpoint out;
  out.host = reader.string(4096);
  out.port = reader.u16();
  return out;
}

net::Lane get_lane(net::Reader& reader) {
  const auto lane = reader.u8();
  if (lane != static_cast<uint8_t>(net::Lane::control) && lane != static_cast<uint8_t>(net::Lane::data))
    throw net::DecodeError("a path is for CONTROL or DATA");
  return static_cast<net::Lane>(lane);
}

bool get_flag(net::Reader& reader) {
  const auto flag = reader.u8();
  if (flag > 1) throw net::DecodeError("bad flag");
  return flag != 0;
}

net::PathToken random_token() {
  const auto bytes = identity::random_bytes(16);
  net::PathToken out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

uint64_t random_circuit() {
  for (;;) {
    const auto bytes = identity::random_bytes(8);
    uint64_t out = 0;
    for (const auto byte : bytes) out = (out << 8) | byte;
    if (out) return out;
  }
}

template <class Decoded, class F>
Decoded decoding(std::span<const uint8_t> bytes, F&& f) {
  net::Reader reader(bytes);
  auto out = f(reader);
  reader.finish();
  return out;
}

// Several connections leave from one local port: the punch port.
void reuse(int fd) {
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
}

asio::ip::tcp::endpoint resolve(asio::io_context& io, const net::Endpoint& to) {
  asio::ip::tcp::resolver resolver(io);
  const auto results = resolver.resolve(to.host, std::to_string(to.port));
  if (results.empty()) throw std::runtime_error("cannot resolve " + to.host);
  return *results.begin();
}

asio::ip::tcp::endpoint any_on(const asio::ip::tcp::endpoint& to, uint16_t port) {
  return {to.protocol() == asio::ip::tcp::v6() ? asio::ip::address(asio::ip::address_v6::any()) : asio::ip::address(asio::ip::address_v4::any()), port};
}

net::Handled reply(net::MessageType type, net::Bytes payload) { return net::Handled::processed({type, std::move(payload), {}}); }

net::Handled punch_refused(const std::string& why) { return reply(m::punch_reply, encode(PunchReply{false, why, {}, {}})); }

}  // namespace

// --- Payloads -----------------------------------------------------------------

net::Bytes encode(const PunchRequest& value) {
  net::Writer writer;
  writer.fixed(value.target.bytes);
  writer.u8(static_cast<uint8_t>(value.lane));
  put(writer, value.from);
  return writer.take();
}

net::Bytes encode(const PunchGo& value) {
  net::Writer writer;
  writer.fixed(value.peer.bytes);
  writer.u8(static_cast<uint8_t>(value.lane));
  writer.fixed(value.token);
  put(writer, value.at);
  return writer.take();
}

net::Bytes encode(const PunchReply& value) {
  net::Writer writer;
  writer.u8(value.accepted ? 1 : 0);
  writer.string(value.why);
  put(writer, value.at);
  writer.fixed(value.token);
  return writer.take();
}

net::Bytes encode(const RelayOpen& value) {
  net::Writer writer;
  writer.fixed(value.target.bytes);
  writer.u8(static_cast<uint8_t>(value.lane));
  return writer.take();
}

net::Bytes encode(const RelayOpenReply& value) {
  net::Writer writer;
  writer.u64(value.circuit);
  writer.string(value.why);
  writer.fixed(value.token);
  return writer.take();
}

net::Bytes encode(const RelayOffer& value) {
  net::Writer writer;
  writer.u64(value.circuit);
  writer.fixed(value.from.bytes);
  writer.u8(static_cast<uint8_t>(value.lane));
  writer.fixed(value.token);
  return writer.take();
}

net::Bytes encode(const RelayOfferReply& value) {
  net::Writer writer;
  writer.u8(value.accepted ? 1 : 0);
  writer.string(value.why);
  return writer.take();
}

net::Bytes encode(const RelayData& value) {
  net::Writer writer;
  writer.u64(value.circuit);
  writer.u64(value.sequence);
  writer.bytes(value.bytes);
  return writer.take();
}

net::Bytes encode(const RelayClose& value) {
  net::Writer writer;
  writer.u64(value.circuit);
  writer.string(value.why);
  writer.u64(value.chunks);
  return writer.take();
}

net::Bytes encode(const RelayCredit& value) {
  net::Writer writer;
  writer.u64(value.circuit);
  writer.u64(value.chunks);
  return writer.take();
}

PunchRequest decode_punch_request(std::span<const uint8_t> bytes) {
  return decoding<PunchRequest>(bytes, [](net::Reader& reader) {
    PunchRequest out;
    out.target.bytes = reader.fixed<16>();
    out.lane = get_lane(reader);
    out.from = get_endpoint(reader);
    return out;
  });
}

PunchGo decode_punch_go(std::span<const uint8_t> bytes) {
  return decoding<PunchGo>(bytes, [](net::Reader& reader) {
    PunchGo out;
    out.peer.bytes = reader.fixed<16>();
    out.lane = get_lane(reader);
    out.token = reader.fixed<16>();
    out.at = get_endpoint(reader);
    return out;
  });
}

PunchReply decode_punch_reply(std::span<const uint8_t> bytes) {
  return decoding<PunchReply>(bytes, [](net::Reader& reader) {
    PunchReply out;
    out.accepted = get_flag(reader);
    out.why = reader.string(4096);
    out.at = get_endpoint(reader);
    out.token = reader.fixed<16>();
    return out;
  });
}

RelayOpen decode_relay_open(std::span<const uint8_t> bytes) {
  return decoding<RelayOpen>(bytes, [](net::Reader& reader) {
    RelayOpen out;
    out.target.bytes = reader.fixed<16>();
    out.lane = get_lane(reader);
    return out;
  });
}

RelayOpenReply decode_relay_open_reply(std::span<const uint8_t> bytes) {
  return decoding<RelayOpenReply>(bytes, [](net::Reader& reader) {
    RelayOpenReply out;
    out.circuit = reader.u64();
    out.why = reader.string(4096);
    out.token = reader.fixed<16>();
    return out;
  });
}

RelayOffer decode_relay_offer(std::span<const uint8_t> bytes) {
  return decoding<RelayOffer>(bytes, [](net::Reader& reader) {
    RelayOffer out;
    out.circuit = reader.u64();
    out.from.bytes = reader.fixed<16>();
    out.lane = get_lane(reader);
    out.token = reader.fixed<16>();
    return out;
  });
}

RelayOfferReply decode_relay_offer_reply(std::span<const uint8_t> bytes) {
  return decoding<RelayOfferReply>(bytes, [](net::Reader& reader) {
    RelayOfferReply out;
    out.accepted = get_flag(reader);
    out.why = reader.string(4096);
    return out;
  });
}

RelayData decode_relay_data(std::span<const uint8_t> bytes) {
  return decoding<RelayData>(bytes, [](net::Reader& reader) {
    RelayData out;
    out.circuit = reader.u64();
    out.sequence = reader.u64();
    out.bytes = reader.bytes(net::max_frame_size);
    return out;
  });
}

RelayClose decode_relay_close(std::span<const uint8_t> bytes) {
  return decoding<RelayClose>(bytes, [](net::Reader& reader) {
    RelayClose out;
    out.circuit = reader.u64();
    out.why = reader.string(4096);
    out.chunks = reader.u64();
    return out;
  });
}

RelayCredit decode_relay_credit(std::span<const uint8_t> bytes) {
  return decoding<RelayCredit>(bytes, [](net::Reader& reader) {
    RelayCredit out;
    out.circuit = reader.u64();
    out.chunks = reader.u64();
    return out;
  });
}

// --- Observation and the simultaneous open ------------------------------------

membership::PathClass classify(const Observation& observation) {
  const auto& seen = observation.seen;
  if (seen.size() >= 2) {
    const bool same = std::all_of(seen.begin(), seen.end(), [&](const net::Endpoint& e) { return e.port == seen.front().port; });
    return same ? membership::PathClass::punchable : membership::PathClass::relay_only;
  }
  if (seen.size() == 1 && seen.front().port == observation.local_port) return membership::PathClass::punchable;
  return membership::PathClass::relay_only;
}

std::pair<uint16_t, net::Endpoint> observe_from(asio::io_context& io, const net::Credentials& key, const NodeRecord& local, uint16_t port,
                                                const net::Endpoint& to, const identity::NodeId& expected, std::chrono::milliseconds timeout) {
  const auto remote = resolve(io, to);
  // Everything on one strand, so the timeout's close never races the
  // connect or the handshake.
  struct State {
    asio::strand<asio::any_io_executor> strand;
    asio::ip::tcp::socket tcp;
    std::optional<net::Session::Socket> socket;
    asio::steady_timer timer;
    std::promise<net::Established> done;
    bool finished = false;
    explicit State(asio::io_context& io) : strand(asio::make_strand(io.get_executor())), tcp(strand), timer(strand) {}
    void close() {
      boost::system::error_code ignored;
      tcp.close(ignored);
      if (socket) socket->close(ignored);
    }
    void fail(const std::string& why) {
      if (finished) return;
      finished = true;
      timer.cancel();
      close();
      done.set_exception(std::make_exception_ptr(std::runtime_error(why)));
    }
  };
  auto state = std::make_shared<State>(io);
  auto future = state->done.get_future();
  boost::system::error_code error;
  state->tcp.open(remote.protocol(), error);
  if (!error) reuse(state->tcp.native_handle());
  if (!error) state->tcp.bind(any_on(remote, port), error);
  if (error) throw std::runtime_error("cannot bind the punch port: " + error.message());
  const uint16_t bound = state->tcp.local_endpoint().port();
  asio::dispatch(state->strand, [state, remote, timeout, key, local, bound] {
    state->timer.expires_after(timeout);
    state->timer.async_wait([state, bound](const boost::system::error_code& timer_error) {
      if (!timer_error) state->fail(std::string("timed out at ") + (state->socket ? "the handshake" : "connect") + " from local port " + std::to_string(bound));
    });
    state->tcp.async_connect(remote, [state, key, local](const boost::system::error_code& connect_error) {
      if (state->finished) return;
      if (connect_error) return state->fail("connect: " + connect_error.message());
      const auto family = state->tcp.local_endpoint().protocol().family();
      state->socket.emplace(state->strand, net::Session::Socket::protocol_type(family, SOCK_STREAM), state->tcp.release());
      net::async_dial_handshake(*state->socket, key, local, net::min_frame_size, net::Lane::probe,
                                [state](std::exception_ptr refusal, net::Established established) {
                                  if (state->finished) return;
                                  if (refusal) {
                                    try {
                                      std::rethrow_exception(refusal);
                                    } catch (const std::exception& refused) {
                                      return state->fail(std::string("handshake: ") + refused.what());
                                    }
                                  }
                                  state->finished = true;
                                  state->timer.cancel();
                                  state->close();
                                  state->done.set_value(std::move(established));
                                });
    });
  });
  // The state's own timer answers at `timeout`; the margin covers only its
  // scheduling, which a loaded or instrumented process can stretch.
  if (future.wait_for(timeout * 2 + std::chrono::seconds(1)) != std::future_status::ready) throw std::runtime_error("observation did not finish");
  const auto established = future.get();
  if (established.peer.id != expected) throw std::runtime_error("reached node " + to_string(established.peer.id) + ", not " + to_string(expected));
  return {bound, established.observed};
}

void simultaneous_open(asio::io_context& io, uint16_t port, const net::Endpoint& to, std::chrono::milliseconds within,
                       std::function<void(int descriptor, int family, std::string why)> done) {
  struct Opening : std::enable_shared_from_this<Opening> {
    asio::strand<asio::any_io_executor> strand;
    asio::ip::tcp::socket tcp;
    asio::steady_timer timer;
    asio::ip::tcp::endpoint remote;
    uint16_t port;
    std::chrono::steady_clock::time_point until;
    std::function<void(int, int, std::string)> done;
    std::string last_error = "no attempt";
    uint64_t attempts = 0;
    bool connecting = false;
    // Where nothing drops a SYN, the ends meet only if their attempts overlap within
    // a round trip; retried at a fixed interval, two ends out of step stay so. A
    // random interval puts each retry at a fresh offset; 1 to 9 ms keeps the old
    // mean of 5, so a port nobody holds (a node down) costs no more than it did.
    std::minstd_rand jitter{std::random_device{}()};

    Opening(asio::io_context& io, asio::ip::tcp::endpoint to, uint16_t local_port, std::chrono::milliseconds within,
            std::function<void(int, int, std::string)> then)
        : strand(asio::make_strand(io.get_executor())),
          tcp(strand),
          timer(strand),
          remote(to),
          port(local_port),
          until(std::chrono::steady_clock::now() + within),
          done(std::move(then)) {}

    // On the strand: one attempt at a time, each bound to the punch port.
    void attempt() {
      if (std::chrono::steady_clock::now() >= until)
        return done(-1, 0, "the simultaneous open did not complete after " + std::to_string(attempts) + " attempts: " + last_error);
      boost::system::error_code error;
      tcp.open(remote.protocol(), error);
      if (!error) reuse(tcp.native_handle());
      if (!error) tcp.bind(any_on(remote, port), error);
      if (error) {
        last_error = "bind: " + error.message();
        boost::system::error_code ignored;
        tcp.close(ignored);
        return again(std::chrono::milliseconds(100));
      }
      connecting = true;
      ++attempts;
      timer.expires_after(std::chrono::milliseconds(700));
      timer.async_wait([self = shared_from_this()](const boost::system::error_code& timer_error) {
        if (timer_error || !self->connecting) return;
        boost::system::error_code ignored;
        self->tcp.close(ignored);
      });
      tcp.async_connect(remote, [self = shared_from_this()](const boost::system::error_code& connect_error) {
        self->connecting = false;
        self->timer.cancel();
        if (!connect_error) {
          // The warning, if any, comes from the node's first ordinary socket.
          net::peer_socket_options(self->tcp.native_handle(), status::ScopedLogger(status::null_sink(), "net"));
          const int family = self->tcp.local_endpoint().protocol().family();
          return self->done(self->tcp.release(), family, "");
        }
        self->last_error = connect_error == asio::error::operation_aborted ? "timed out" : connect_error.message();
        boost::system::error_code ignored;
        self->tcp.close(ignored);
        // A refusal is the far end's kernel answering before its own connect has begun:
        // on a path with nothing to drop the SYN, the two ends meet only if their
        // attempts overlap, so try again soon, at a random moment.
        if (connect_error == asio::error::connection_refused)
          return self->again(std::chrono::microseconds(std::uniform_int_distribution<int>(1000, 9000)(self->jitter)));
        self->again(std::chrono::milliseconds(100));
      });
    }
    void again(std::chrono::microseconds after) {
      timer.expires_after(after);
      timer.async_wait([self = shared_from_this()](const boost::system::error_code&) { self->attempt(); });
    }
  };
  asio::ip::tcp::endpoint remote;
  try {
    remote = resolve(io, to);
  } catch (const std::exception& error) {
    return asio::post(io, [done, why = std::string(error.what())] { done(-1, 0, why); });
  }
  auto opening = std::make_shared<Opening>(io, remote, port, within, std::move(done));
  asio::dispatch(opening->strand, [opening] { opening->attempt(); });
}

// --- A circuit's end ----------------------------------------------------------

// The pump between this node's end of a circuit and the relay: bytes the
// session writes go to the relay as relay_data, in order; bytes the relay
// forwards are written to the session, in order. Everything on its strand.
struct Paths::Owner {
  std::mutex mutex;
  Paths* paths = nullptr;
};

void Paths::with(const std::shared_ptr<Owner>& owner, const std::function<void(Paths&)>& f) {
  std::lock_guard lock(owner->mutex);
  if (owner->paths) f(*owner->paths);
}

class Paths::Pump : public std::enable_shared_from_this<Pump> {
 public:
  Pump(asio::io_context& io, std::shared_ptr<Paths::Owner> owner, uint64_t circuit, identity::NodeId relay, identity::NodeId far_end, net::Lane lane,
       net::Lane carrier, int descriptor)
      : _strand(asio::make_strand(io.get_executor())),
        _socket(_strand, net::Session::Socket::protocol_type(AF_UNIX, SOCK_STREAM), descriptor),
        _owner(std::move(owner)),
        _circuit(circuit),
        _relay(relay),
        _far_end(far_end),
        _lane(lane),
        _carrier(carrier),
        _frame(lane == net::Lane::data ? sched::FrameType::loader : sched::FrameType::control) {}

  const identity::NodeId& relay() const noexcept { return _relay; }
  net::Lane carrier() const noexcept { return _carrier; }
  // How many of this end's chunks the relay's session has taken.
  uint64_t chunks_sent() const noexcept { return _taken.load(); }

  void start() {
    asio::dispatch(_strand, [self = shared_from_this()] { self->read(); });
  }
  // In the far end's order, whichever sessions the chunks came by. Nothing in order
  // can be further behind than the window, so neither can what waits here.
  void deliver(uint64_t sequence, net::Bytes bytes) {
    asio::post(_strand, [self = shared_from_this(), sequence, bytes = std::move(bytes)]() mutable {
      if (self->_closed || sequence < self->_next_in) return;
      self->_last_arrival = std::chrono::steady_clock::now();
      self->far_end_moved();
      if (sequence != self->_next_in) {
        if (self->_early.size() >= relay_window_chunks) return self->ended("the relay's chunks came further out of order than the window");
        self->_early.emplace(sequence, std::move(bytes));
        return;
      }
      const bool idle = self->_out.empty();
      self->_out.push_back(std::move(bytes));
      ++self->_next_in;
      for (auto next = self->_early.find(self->_next_in); next != self->_early.end(); next = self->_early.find(self->_next_in)) {
        self->_out.push_back(std::move(next->second));
        self->_early.erase(next);
        ++self->_next_in;
      }
      if (idle) self->write();
    });
  }
  // The far end took `chunks` more off the circuit.
  void credit(uint64_t chunks) {
    asio::post(_strand, [self = shared_from_this(), chunks] {
      self->_unanswered -= std::min(chunks, self->_unanswered);
      self->far_end_moved();
      self->read();
    });
  }
  // Now: what has not reached the session is lost.
  void close() {
    asio::post(_strand, [self = shared_from_this()] {
      self->_closed = true;
      boost::system::error_code ignored;
      self->_socket.close(ignored);
    });
  }
  // The far end has finished after `chunks`: they reach the session first.
  void finish(uint64_t chunks, std::chrono::milliseconds bound) {
    asio::post(_strand, [self = shared_from_this(), chunks, bound] {
      self->_finishing = true;
      self->_finish_after = chunks;
      self->_last_arrival = std::chrono::steady_clock::now();
      self->close_if_finished();
      self->bound_finishing(bound);
    });
  }

 private:
  // Waiting for chunks that may have died with a session: none for the bound, and it
  // closes as if the count had been unknown.
  void bound_finishing(std::chrono::milliseconds bound) {
    if (_closed) return;
    auto timer = std::make_shared<asio::steady_timer>(_strand, bound);
    timer->async_wait([timer, self = shared_from_this(), bound](const boost::system::error_code& error) {
      if (error || self->_closed) return;
      if (std::chrono::steady_clock::now() - self->_last_arrival < bound) return self->bound_finishing(bound);
      if (self->_finish_after != unknown_chunks && self->_next_in < self->_finish_after)
        Paths::with(self->_owner, [](Paths& paths) { paths.count(&PathStats::finishes_expired); });
      self->_finish_after = unknown_chunks;
      self->close_if_finished();
    });
  }
  void close_if_finished() {
    if (!_finishing || !_out.empty() || (_finish_after != unknown_chunks && _next_in < _finish_after)) return;
    close();
    Paths::with(_owner, [&](Paths& paths) { paths.forget_end(_circuit, this); });
  }
  void far_end_moved() {
    Paths::with(_owner, [&](Paths& paths) {
      paths._client.note_path_progress(_far_end, _lane);
      paths._client.note_path_progress(_relay, _carrier);
    });
  }
  // One read at a time, none past the window, and none while the relay's session
  // has a few chunks of this circuit's already: the session writing into the far
  // side of the socket feels a slow circuit as a slow socket.
  void read() {
    if (_closed || _draining || _reading || _unanswered >= relay_window_chunks || _waiting.size() + _sending >= relay_pipeline_chunks) return;
    _reading = true;
    _socket.async_read_some(asio::buffer(_buffer), [self = shared_from_this()](const boost::system::error_code& error, size_t got) {
      self->_reading = false;
      if (error && self->_closed) return self->ended("closed");
      if (error) {
        // The session's end has closed: what it wrote before closing still goes, then the close.
        self->_draining = true;
        self->_drained_why = "the session's end closed: " + error.message();
        return self->end_if_drained();
      }
      ++self->_unanswered;
      self->_waiting.push_back(
          RelayData{self->_circuit, self->_next_out++, net::Bytes(self->_buffer.begin(), self->_buffer.begin() + static_cast<std::ptrdiff_t>(got))});
      self->carry();
      self->read();
    });
  }
  void end_if_drained() {
    if (_draining && _waiting.empty() && _sending == 0) ended(_drained_why, true);
  }
  // In order: the session's queue keeps notifications of one class first in, first out.
  void carry() {
    while (!_closed && !_waiting.empty() && _sending < relay_pipeline_chunks) {
      auto outcome = net::Client::Paced::gone;
      Paths::with(_owner, [&](Paths& paths) {
        outcome = paths._client.send_paced(_relay, _carrier, {m::relay_data, encode(_waiting.front()), {}}, _frame, _strand,
                                           [self = shared_from_this()](bool written) {
                                             --self->_sending;
                                             if (!written) return self->ended("the relay's session ended");
                                             self->carry();
                                             self->read();
                                             self->end_if_drained();
                                           });
      });
      if (outcome == net::Client::Paced::gone) return ended("the relay could not take the circuit's bytes");
      if (outcome == net::Client::Paced::full) {
        if (!_retrying) {
          _retrying = true;
          later([](Pump& pump) {
            pump._retrying = false;
            pump.carry();
          });
        }
        return;
      }
      _waiting.pop_front();
      ++_sending;
      ++_taken;
    }
  }
  void write() {
    asio::async_write(_socket, asio::buffer(_out.front()), [self = shared_from_this()](const boost::system::error_code& error, size_t) {
      // The session's end has gone: nothing more reaches it, but what this end sent the
      // relay is still known exactly, so it drains as at end-of-file.
      if (error && self->_closed) return;
      if (error) {
        self->_out.clear();
        self->_draining = true;
        if (self->_drained_why.empty()) self->_drained_why = "the session's end closed: " + error.message();
        return self->end_if_drained();
      }
      self->_out.pop_front();
      ++self->_owed;
      self->pay();
      if (!self->_out.empty()) return self->write();
      self->close_if_finished();
    });
  }
  // Credit for what has reached the session, a batch at a time. The window
  // is several batches, so a sender out of credit always has a batch owing.
  void pay() {
    if (_closed || _paying || _owed < relay_credit_batch) return;
    auto outcome = net::Client::Paced::gone;
    Paths::with(_owner, [&](Paths& paths) {
      outcome = paths._client.send_paced(_relay, net::Lane::control, {m::relay_credit, encode(RelayCredit{_circuit, _owed}), {}}, sched::FrameType::control,
                                         _strand, [self = shared_from_this()](bool written) {
                                           self->_paying = false;
                                           if (!written) return self->ended("the relay's session ended");
                                           self->pay();
                                         });
    });
    if (outcome == net::Client::Paced::gone) return ended("the relay could not take the circuit's credit");
    if (outcome == net::Client::Paced::full) return later([](Pump& pump) { pump.pay(); });
    _paying = true;
    _owed = 0;
  }
  void later(void (*again)(Pump&)) {
    auto timer = std::make_shared<asio::steady_timer>(_strand, std::chrono::milliseconds(5));
    timer->async_wait([timer, self = shared_from_this(), again](const boost::system::error_code& error) {
      if (!error) again(*self);
    });
  }
  // `exact`: every chunk taken was written, so the count is what the far end will get.
  // Otherwise a chunk may have died with a session, and the far end must not wait for it.
  void ended(const std::string& why, bool exact = false) {
    if (_ended) return;
    _ended = true;
    // Closed here already, by a close or a finish: no failure to count.
    const bool failed = !exact && !_closed;
    _closed = true;
    boost::system::error_code ignored;
    _socket.close(ignored);
    const auto chunks = exact ? _taken.load() : unknown_chunks;
    Paths::with(_owner, [&](Paths& paths) {
      if (failed) paths.count(&PathStats::closes_unknown);
      paths.close_end(_circuit, why, true, chunks);
    });
  }

  asio::strand<asio::any_io_executor> _strand;
  net::Session::Socket _socket;
  const std::shared_ptr<Paths::Owner> _owner;
  const uint64_t _circuit;
  const identity::NodeId _relay;
  const identity::NodeId _far_end;
  const net::Lane _lane;
  // The lane to the relay this circuit's bytes and its close go on, fixed at
  // the start: they may not overtake each other.
  const net::Lane _carrier;
  const sched::FrameType _frame;
  std::array<uint8_t, relay_chunk_bytes> _buffer{};
  std::deque<net::Bytes> _out;
  // Chunks read and not yet taken by the relay's session, and taken and not yet written.
  std::deque<RelayData> _waiting;
  uint64_t _sending = 0;
  bool _retrying = false;
  // Numbering in each direction, what came early, and when the far end is done.
  uint64_t _next_out = 0;
  std::atomic<uint64_t> _taken{0};
  uint64_t _next_in = 0;
  std::map<uint64_t, net::Bytes> _early;
  uint64_t _finish_after = unknown_chunks;
  std::chrono::steady_clock::time_point _last_arrival{};
  bool _draining = false;
  std::string _drained_why;
  // Chunks sent that the far end has not yet given credit for, and chunks
  // this end has written and not yet given credit for.
  uint64_t _unanswered = 0;
  uint64_t _owed = 0;
  bool _reading = false;
  bool _paying = false;
  bool _finishing = false;
  bool _closed = false;
  bool _ended = false;
};

// --- Paths --------------------------------------------------------------------

Paths::Paths(asio::io_context& io, PathsConfig config, net::Client& client, std::function<NodeRecord()> self, Roster roster, Caller caller)
    : _io(io),
      _config(std::move(config)),
      _owner(std::make_shared<Owner>()),
      _client(client),
      _self(std::move(self)),
      _roster(std::move(roster)),
      _caller(std::move(caller)) {
  _owner->paths = this;
  for (int i = 0; i < 2; ++i)
    _workers.emplace_back(
        [this](std::stop_token stop) { supervise::supervised_loop(_config.threads, "reach-path-worker", stop, [this, stop] { work(stop); }); });
}

Paths::~Paths() { stop(); }

void Paths::stop() {
  std::map<uint64_t, std::shared_ptr<Pump>> ends;
  {
    contract::Lock lock(_mutex);
    if (_stopped && _workers.empty()) return;
    _stopped = true;
    ends.swap(_ends);
    _circuits.clear();
  }
  _wake.notify_all();
  for (auto& worker : _workers) {
    worker.request_stop();
    if (worker.joinable()) worker.join();
  }
  _workers.clear();
  {
    std::lock_guard lock(_owner->mutex);
    _owner->paths = nullptr;
  }
  for (auto& [_, pump] : ends) pump->close();
}

void Paths::work(std::stop_token stop) {
  for (;;) {
    std::function<void()> job;
    {
      contract::Lock lock(_mutex);
      _wake.wait(lock.native(), stop, [&]() DANNAN_REQUIRES(_mutex) { return _stopped || !_queue.empty(); });
      if (_stopped || stop.stop_requested()) return;
      job = std::move(_queue.front());
      _queue.pop_front();
    }
    job();
  }
}

std::optional<NodeRecord> Paths::known(const identity::NodeId& id) const {
  for (auto& node : _roster())
    if (node.id == id) return node;
  return std::nullopt;
}

void Paths::make_path(const identity::NodeId& peer, net::Lane lane, std::function<void(std::string)> failed) {
  {
    contract::Lock lock(_mutex);
    if (_stopped) return asio::post(_io, [failed] { failed("the node is stopping"); });
    _queue.push_back([this, peer, lane, failed] {
      const auto self = _self();
      const auto record = known(peer);
      if (!record) return failed("the peer is not known");
      std::string why;
      if (self.path_class != membership::PathClass::relay_only && record->path_class != membership::PathClass::relay_only) {
        why = try_punch(self, *record, lane);
        if (why.empty()) return;
        why = "punch: " + why + "; ";
      }
      const auto relayed = try_relay(*record, lane);
      if (relayed.empty()) return;
      failed(why + "relay: " + relayed);
    });
  }
  _wake.notify_one();
}

std::string Paths::adopt_and_wait(int descriptor, int family, const identity::NodeId& peer, net::Lane lane, net::Role role, net::Purpose purpose,
                                  net::PathToken token) {
  auto verdict = std::make_shared<std::promise<std::string>>();
  auto future = verdict->get_future();
  _client.adopt(descriptor, family, peer, lane, role, purpose, token, [verdict](std::string why) { verdict->set_value(std::move(why)); });
  if (future.wait_for(_config.connect_timeout + std::chrono::seconds(2)) != std::future_status::ready) return "the handshake did not finish";
  return future.get();
}

std::string Paths::try_punch(const NodeRecord& self, const NodeRecord& peer, net::Lane lane) {
  std::string why = "no broker";
  for (const auto& broker : _roster()) {
    if (broker.id == self.id || broker.id == peer.id || !broker.inbound_capable() || !_client.has_route(broker.id, net::Lane::control)) continue;
    {
      contract::Lock lock(_mutex);
      ++_stats.punches_tried;
    }
    try {
      const auto [port, seen] = observe_from(_io, _config.credentials, self, 0, {broker.host, broker.port}, broker.id, _config.connect_timeout);
      const auto outcome = _caller(broker, m::punch_request, encode(PunchRequest{peer.id, lane, seen}), _config.connect_timeout * 3);
      if (!outcome.reply || outcome.reply->type != m::punch_reply) {
        why = "broker " + to_string(broker.id) + ": " + (outcome.reply ? "refused" : outcome.failure);
        continue;
      }
      const auto answer = decode_punch_reply(outcome.reply->payload);
      if (!answer.accepted) {
        why = "broker " + to_string(broker.id) + ": " + answer.why;
        continue;
      }
      auto opened = std::make_shared<std::promise<std::tuple<int, int, std::string>>>();
      auto future = opened->get_future();
      simultaneous_open(_io, port, answer.at, _config.punch_window,
                        [opened](int descriptor, int family, std::string reason) { opened->set_value({descriptor, family, std::move(reason)}); });
      auto [descriptor, family, failure] = future.get();
      if (descriptor < 0) {
        why = failure;
        continue;
      }
      const auto role = self.id < peer.id ? net::Role::dialler : net::Role::acceptor;
      why = adopt_and_wait(descriptor, family, peer.id, lane, role, net::Purpose::punched, answer.token);
      if (why.empty()) {
        contract::Lock lock(_mutex);
        ++_stats.punches_made;
        return {};
      }
    } catch (const std::exception& error) {
      why = "broker " + to_string(broker.id) + ": " + error.what();
    }
  }
  return why;
}

std::string Paths::try_relay(const NodeRecord& peer, net::Lane lane) {
  std::string why = "no relay";
  const auto self = _self();
  for (const auto& relay : _roster()) {
    if (relay.id == self.id || relay.id == peer.id || !relay.relays() || !_client.has_route(relay.id, net::Lane::control)) continue;
    {
      contract::Lock lock(_mutex);
      ++_stats.relays_tried;
    }
    try {
      if (lane == net::Lane::data) {
        _client.open_lane(relay, net::Lane::data);
        await_data_lane(relay.id);
      }
      const auto outcome = _caller(relay, m::relay_open, encode(RelayOpen{peer.id, lane}), _config.connect_timeout * 3);
      if (!outcome.reply || outcome.reply->type != m::relay_open_reply) {
        why = "relay " + to_string(relay.id) + ": " + (outcome.reply ? "refused" : outcome.failure);
        continue;
      }
      const auto answer = decode_relay_open_reply(outcome.reply->payload);
      if (!answer.circuit) {
        why = "relay " + to_string(relay.id) + ": " + answer.why;
        continue;
      }
      int session_end = -1;
      int family = AF_UNIX;
      const auto pump = open_end(answer.circuit, relay.id, peer.id, lane, &session_end, &family);
      if (!pump) {
        why = "the node is stopping";
        break;
      }
      why = adopt_and_wait(session_end, family, peer.id, lane, net::Role::dialler, net::Purpose::relayed, answer.token);
      if (why.empty()) {
        contract::Lock lock(_mutex);
        ++_stats.relays_made;
        return {};
      }
      close_end(answer.circuit, why, true);
    } catch (const std::exception& error) {
      why = "relay " + to_string(relay.id) + ": " + error.what();
    }
  }
  return why;
}

std::shared_ptr<Paths::Pump> Paths::open_end(uint64_t circuit, const identity::NodeId& relay, const identity::NodeId& far_end, net::Lane lane, int* session_end,
                                             int* family) {
  int pair[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) throw std::runtime_error("socketpair failed");
#ifdef SO_NOSIGPIPE
  // As a session's: the session at the other end going away must never end the process.
  const int on = 1;
  for (const int fd : pair) ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
  const auto carrier = carrier_to(relay, lane);
  auto pump = std::make_shared<Pump>(_io, _owner, circuit, relay, far_end, lane, carrier, pair[1]);
  {
    contract::Lock lock(_mutex);
    if (_stopped) {
      ::close(pair[0]);
      pump->close();
      return nullptr;
    }
    _ends[circuit] = pump;
  }
  pump->start();
  *session_end = pair[0];
  *family = AF_UNIX;
  return pump;
}

void Paths::close_end(uint64_t circuit, const std::string& why, bool tell_relay, uint64_t chunks) {
  std::shared_ptr<Pump> pump;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _ends.find(circuit); found != _ends.end()) {
      pump = found->second;
      // Finishing, it stays to be found: the chunks it still waits for may arrive after
      // the close, by another session. It leaves when it has them.
      if (tell_relay) _ends.erase(found);
    }
  }
  if (!pump) return;
  if (!tell_relay) return pump->finish(chunks, _config.finish_wait);
  pump->close();
  send_until_taken(pump->relay(), pump->carrier(), {m::relay_close, encode(RelayClose{circuit, why, chunks}), {}});
}

void Paths::forget_end(uint64_t circuit, const Pump* pump) {
  contract::Lock lock(_mutex);
  if (const auto found = _ends.find(circuit); found != _ends.end() && found->second.get() == pump) _ends.erase(found);
}

void Paths::shortly_after(std::chrono::milliseconds delay, std::function<void(Paths&)> f) {
  auto timer = std::make_shared<asio::steady_timer>(_io, delay);
  timer->async_wait([timer, owner = _owner, f = std::move(f)](const boost::system::error_code& error) {
    if (!error) with(owner, f);
  });
}

void Paths::watch_close(uint64_t circuit, bool toward_b) {
  shortly_after(_config.finish_wait, [circuit, toward_b](Paths& paths) {
    paths.advance(circuit, toward_b);
    bool held = false;
    {
      contract::Lock lock(paths._mutex);
      if (const auto found = paths._circuits.find(circuit); found != paths._circuits.end())
        held = (toward_b ? found->second.to_b : found->second.to_a).close.has_value();
    }
    if (held) paths.watch_close(circuit, toward_b);
  });
}

void Paths::count(uint64_t PathStats::* counter) {
  contract::Lock lock(_mutex);
  ++(_stats.*counter);
}

void Paths::shortly(std::function<void(Paths&)> f) {
  auto timer = std::make_shared<asio::steady_timer>(_io, std::chrono::milliseconds(5));
  timer->async_wait([timer, owner = _owner, f = std::move(f)](const boost::system::error_code& error) {
    if (!error) with(owner, f);
  });
}

void Paths::send_until_taken(identity::NodeId peer, net::Lane lane, net::Message message) {
  const auto outcome = _client.send_paced(peer, lane, message, sched::FrameType::control, _io.get_executor(), {});
  if (outcome == net::Client::Paced::full) shortly([peer, lane, message = std::move(message)](Paths& paths) { paths.send_until_taken(peer, lane, message); });
}

void Paths::await_data_lane(const identity::NodeId& peer) const {
  const auto until = std::chrono::steady_clock::now() + _config.connect_timeout;
  while (!_client.has_route(peer, net::Lane::data) && std::chrono::steady_clock::now() < until) {
    {
      contract::Lock lock(_mutex);
      if (_stopped) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

net::Lane Paths::carrier_to(const identity::NodeId& peer, net::Lane lane) const {
  return lane == net::Lane::data && _client.has_route(peer, net::Lane::data) ? net::Lane::data : net::Lane::control;
}

net::Handled Paths::punch_request(const NodeRecord& from, const net::Message& request) {
  PunchRequest asked;
  try {
    asked = decode_punch_request(request.payload);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  if (!_self().inbound_capable()) return punch_refused("this node is no broker: it takes no inbound connection");
  const auto target = known(asked.target);
  if (!target || !_client.has_route(asked.target, net::Lane::control)) return punch_refused("the broker has no session with the target");
  const auto token = random_token();
  const auto outcome = _caller(*target, m::punch_go, encode(PunchGo{from.id, asked.lane, token, asked.from}), _config.connect_timeout * 2);
  if (!outcome.reply || outcome.reply->type != m::punch_reply)
    return punch_refused("the target did not answer: " + (outcome.reply ? std::string("refused") : outcome.failure));
  PunchReply answer;
  try {
    answer = decode_punch_reply(outcome.reply->payload);
  } catch (const net::DecodeError& error) {
    return punch_refused(std::string("the target's answer: ") + error.what());
  }
  if (!answer.accepted) return punch_refused("the target declined: " + answer.why);
  {
    contract::Lock lock(_mutex);
    ++_stats.punches_brokered;
  }
  return reply(m::punch_reply, encode(PunchReply{true, {}, answer.at, token}));
}

net::Handled Paths::punch_go(const NodeRecord& from, const net::Message& request) {
  PunchGo go;
  try {
    go = decode_punch_go(request.payload);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  const auto self = _self();
  if (self.path_class == membership::PathClass::relay_only) return punch_refused("this node's NAT cannot be punched");
  {
    contract::Lock lock(_mutex);
    if (_stopped) return punch_refused("the node is stopping");
  }
  std::pair<uint16_t, net::Endpoint> observed;
  try {
    observed = observe_from(_io, _config.credentials, self, 0, {from.host, from.port}, from.id, _config.connect_timeout);
  } catch (const std::exception& error) {
    return punch_refused(std::string("cannot observe this node's punch port: ") + error.what());
  }
  const auto peer = go.peer;
  const auto lane = go.lane;
  const auto token = go.token;
  const auto role = self.id < peer ? net::Role::dialler : net::Role::acceptor;
  simultaneous_open(_io, observed.first, go.at, _config.punch_window, [owner = _owner, peer, lane, token, role](int descriptor, int family, std::string) {
    if (descriptor < 0) return;
    bool adopted = false;
    with(owner, [&](Paths& paths) {
      adopted = true;
      paths._client.adopt(descriptor, family, peer, lane, role, net::Purpose::punched, token, [owner](std::string why) {
        if (!why.empty()) return;
        with(owner, [](Paths& counted) {
          contract::Lock lock(counted._mutex);
          ++counted._stats.punches_made;
        });
      });
    });
    if (!adopted) ::close(descriptor);
  });
  return reply(m::punch_reply, encode(PunchReply{true, {}, observed.second, {}}));
}

net::Handled Paths::relay_open(const NodeRecord& from, const net::Message& request) {
  RelayOpen asked;
  try {
    asked = decode_relay_open(request.payload);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  const auto refuse = [](const std::string& why) { return reply(m::relay_open_reply, encode(RelayOpenReply{0, why, {}})); };
  if (!_config.relay) return refuse("this node relays for nobody");
  const auto target = known(asked.target);
  if (!target || !_client.has_route(asked.target, net::Lane::control)) return refuse("the relay has no session with the target");
  const auto circuit = random_circuit();
  const auto token = random_token();
  // The opener dialled its DATA lane before asking; it may not have landed here.
  if (asked.lane == net::Lane::data) await_data_lane(from.id);
  const auto carrier_a = carrier_to(from.id, asked.lane);
  {
    contract::Lock lock(_mutex);
    if (_stopped) return refuse("the relay is stopping");
    if (asked.lane == net::Lane::data) {
      const auto data = std::count_if(_circuits.begin(), _circuits.end(), [](const auto& c) { return c.second.lane == net::Lane::data; });
      if (static_cast<size_t>(data) >= _config.max_data_circuits) {
        ++_stats.data_circuits_refused;
        return refuse("the relay's DATA budget is spent");
      }
    }
    // Registered before the offer, so nothing the target sends is lost.
    _circuits[circuit] = Circuit{from.id, asked.target, asked.lane, 0, {}, {}};
    _circuits[circuit].to_a.carrier = carrier_a;
  }
  const auto outcome = _caller(*target, m::relay_offer, encode(RelayOffer{circuit, from.id, asked.lane, token}), _config.connect_timeout * 2);
  std::string why;
  if (!outcome.reply || outcome.reply->type != m::relay_offer_reply) {
    why = "the target did not answer: " + (outcome.reply ? std::string("refused") : outcome.failure);
  } else {
    try {
      const auto answer = decode_relay_offer_reply(outcome.reply->payload);
      if (!answer.accepted) why = "the target declined: " + answer.why;
    } catch (const net::DecodeError& error) {
      why = std::string("the target's answer: ") + error.what();
    }
  }
  if (!why.empty()) {
    contract::Lock lock(_mutex);
    _circuits.erase(circuit);
    return refuse(why);
  }
  // The target has its lane by now, or will not soon. It sends nothing before
  // the opener's hello, which waits on this reply, so nothing has gone yet.
  if (asked.lane == net::Lane::data) await_data_lane(asked.target);
  const auto carrier_b = carrier_to(asked.target, asked.lane);
  {
    contract::Lock lock(_mutex);
    ++_stats.circuits_relayed;
    if (const auto found = _circuits.find(circuit); found != _circuits.end()) found->second.to_b.carrier = carrier_b;
  }
  return reply(m::relay_open_reply, encode(RelayOpenReply{circuit, {}, token}));
}

net::Handled Paths::relay_offer(const NodeRecord& from, const net::Message& request) {
  RelayOffer offer;
  try {
    offer = decode_relay_offer(request.payload);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  const auto refuse = [](const std::string& why) { return reply(m::relay_offer_reply, encode(RelayOfferReply{false, why})); };
  if (offer.lane == net::Lane::data) {
    _client.open_lane(from, net::Lane::data);
    await_data_lane(from.id);
  }
  int session_end = -1;
  int family = AF_UNIX;
  std::shared_ptr<Pump> pump;
  try {
    pump = open_end(offer.circuit, from.id, offer.from, offer.lane, &session_end, &family);
  } catch (const std::exception& error) {
    return refuse(error.what());
  }
  if (!pump) return refuse("the node is stopping");
  const auto circuit = offer.circuit;
  _client.adopt(session_end, family, offer.from, offer.lane, net::Role::acceptor, net::Purpose::relayed, offer.token,
                [owner = _owner, circuit](std::string why) {
                  if (!why.empty()) with(owner, [&](Paths& paths) { paths.close_end(circuit, why, true); });
                });
  return reply(m::relay_offer_reply, encode(RelayOfferReply{true, {}}));
}

net::Handled Paths::relay_data(const NodeRecord& from, const net::Message& request) {
  RelayData data;
  try {
    data = decode_relay_data(request.payload);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  std::shared_ptr<Pump> end;
  std::optional<bool> toward_b;
  bool broke_window = false;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _ends.find(data.circuit); found != _ends.end() && found->second->relay() == from.id) {
      end = found->second;
    } else if (const auto circuit = _circuits.find(data.circuit); circuit != _circuits.end()) {
      auto& c = circuit->second;
      if (from.id == c.a) toward_b = true;
      if (from.id == c.b) toward_b = false;
      if (toward_b) {
        auto& leg = *toward_b ? c.to_b : c.to_a;
        c.bytes += data.bytes.size();
        _stats.relayed_bytes += data.bytes.size();
        broke_window = leg.held.size() >= relay_window_chunks;
        const bool dropped = _config.drop_hook && c.lane == net::Lane::data && _config.drop_hook(data.sequence, false, *toward_b);
        if (!broke_window && !leg.lost && !dropped) {
          leg.held.push_back(std::move(data));
          ++leg.received;
          leg.last_received = std::chrono::steady_clock::now();
        }
      }
    }
  }
  if (end) {
    end->deliver(data.sequence, std::move(data.bytes));
    return net::Handled::processed();
  }
  if (toward_b) _client.note_path_progress(from.id, net::Lane::data);
  if (broke_window) {
    lost(data.circuit, *toward_b);
    return net::Handled::processed();
  }
  if (toward_b) advance(data.circuit, *toward_b);
  return net::Handled::processed();
}

void Paths::advance(uint64_t circuit, bool toward_b) {
  // One thread at a time hands a leg's messages to the session, so they are taken
  // and queued in the same order; another that wants it asks this one to look again.
  {
    contract::Lock lock(_mutex);
    const auto found = _circuits.find(circuit);
    if (found == _circuits.end()) return;
    auto& leg = toward_b ? found->second.to_b : found->second.to_a;
    if (leg.advancing) {
      leg.again = true;
      return;
    }
    leg.advancing = true;
  }
  const auto done = [&] {
    contract::Lock lock(_mutex);
    const auto found = _circuits.find(circuit);
    if (found == _circuits.end()) return true;
    auto& leg = toward_b ? found->second.to_b : found->second.to_a;
    if (std::exchange(leg.again, false)) return false;
    leg.advancing = false;
    return true;
  };
  for (;;) {
    net::Message message;
    identity::NodeId peer;
    auto frame = sched::FrameType::control;
    // Credit goes on CONTROL and overtakes; bytes and the close go on the circuit's
    // carrier. Their order on arrival is the far end's to restore, by number.
    auto lane = net::Lane::control;
    // Taken off the leg before the send: its completion may run on another
    // thread before the send returns. Put back only if the queue refuses it.
    uint64_t credit = 0;
    std::optional<RelayData> chunk;
    std::optional<RelayClose> close;
    bool idle = false;
    {
      contract::Lock lock(_mutex);
      const auto found = _circuits.find(circuit);
      if (found == _circuits.end()) return;
      auto& c = found->second;
      auto& leg = toward_b ? c.to_b : c.to_a;
      peer = toward_b ? c.b : c.a;
      if (leg.lost || leg.in_flight >= relay_pipeline_chunks) {
        idle = true;
      } else if (leg.credit) {
        credit = std::exchange(leg.credit, 0);
        message = {m::relay_credit, encode(RelayCredit{circuit, credit}), {}};
      } else if (!leg.held.empty() && _config.drop_hook && c.lane == net::Lane::data && _config.drop_hook(leg.held.front().sequence, true, toward_b)) {
        leg.held.pop_front();
        idle = true;
        leg.again = true;
      } else if (!leg.held.empty()) {
        lane = leg.carrier;
        chunk = std::move(leg.held.front());
        leg.held.pop_front();
        frame = c.lane == net::Lane::data ? sched::FrameType::loader : sched::FrameType::control;
        message = {m::relay_data, encode(*chunk), {}};
      } else if (leg.close && leg.close->chunks != unknown_chunks && leg.received < leg.close->chunks &&
                 std::chrono::steady_clock::now() - std::max(leg.close_since, leg.last_received) >= _config.finish_wait) {
        // Nothing of its last chunks for the bound, from the close or the last that came:
        // they died with a session. A slow leg that is still moving is not cut.
        leg.close->chunks = unknown_chunks;
        ++_stats.relay_closes_expired;
        idle = false;
        close = std::move(leg.close);
        leg.close.reset();
        frame = sched::FrameType::control;
        lane = leg.carrier;
        message = {m::relay_close, encode(*close), {}};
      } else if (leg.close && (leg.close->chunks == unknown_chunks || leg.received >= leg.close->chunks)) {
        // Only after every chunk the closing end sent: the last may come by another session.
        lane = leg.carrier;
        close = std::move(leg.close);
        leg.close.reset();
        message = {m::relay_close, encode(*close), {}};
      } else {
        idle = true;
      }
      if (!idle) ++leg.in_flight;
    }
    if (idle) {
      if (done()) return;
      continue;
    }
    const bool last = close.has_value();
    const auto outcome = _client.send_paced(peer, lane, std::move(message), frame, _io.get_executor(), [owner = _owner, circuit, toward_b, last](bool written) {
      with(owner, [&](Paths& paths) { paths.advanced(circuit, toward_b, written, last); });
    });
    if (outcome == net::Client::Paced::taken) continue;
    {
      contract::Lock lock(_mutex);
      const auto found = _circuits.find(circuit);
      if (found == _circuits.end()) return;
      auto& leg = toward_b ? found->second.to_b : found->second.to_a;
      --leg.in_flight;
      leg.credit += credit;
      if (chunk) leg.held.push_front(std::move(*chunk));
      if (close) leg.close = std::move(close);
      leg.advancing = false;
      leg.again = false;
    }
    if (outcome == net::Client::Paced::gone) return lost(circuit, toward_b);
    shortly([circuit, toward_b](Paths& paths) { paths.advance(circuit, toward_b); });
    return;
  }
}

void Paths::advanced(uint64_t circuit, bool toward_b, bool written, bool last) {
  if (!written) return lost(circuit, toward_b);
  {
    contract::Lock lock(_mutex);
    const auto found = _circuits.find(circuit);
    if (found == _circuits.end()) return;
    if (last) {
      _circuits.erase(found);
      return;
    }
    auto& leg = toward_b ? found->second.to_b : found->second.to_a;
    if (leg.in_flight) --leg.in_flight;
  }
  advance(circuit, toward_b);
}

void Paths::lost(uint64_t circuit, bool toward_b) {
  {
    contract::Lock lock(_mutex);
    const auto found = _circuits.find(circuit);
    if (found == _circuits.end()) return;
    auto& leg = toward_b ? found->second.to_b : found->second.to_a;
    auto& other = toward_b ? found->second.to_a : found->second.to_b;
    if (!leg.lost) ++_stats.relay_ends_lost;
    leg.lost = true;
    leg.held.clear();
    leg.credit = 0;
    if (other.lost) {
      _circuits.erase(found);
      return;
    }
    // A count it held may include chunks that died with the lost session.
    if (other.close)
      other.close->chunks = unknown_chunks;
    else
      other.close = RelayClose{circuit, "the relay lost the other end", unknown_chunks};
  }
  advance(circuit, !toward_b);
}

net::Handled Paths::relay_credit(const NodeRecord& from, const net::Message& request) {
  RelayCredit credit;
  try {
    credit = decode_relay_credit(request.payload);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  std::shared_ptr<Pump> end;
  std::optional<bool> toward_b;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _ends.find(credit.circuit); found != _ends.end() && found->second->relay() == from.id) {
      end = found->second;
    } else if (const auto circuit = _circuits.find(credit.circuit); circuit != _circuits.end()) {
      auto& c = circuit->second;
      if (from.id == c.a) toward_b = true;
      if (from.id == c.b) toward_b = false;
      if (toward_b && !(*toward_b ? c.to_b : c.to_a).lost) (*toward_b ? c.to_b : c.to_a).credit += credit.chunks;
    }
  }
  if (end) end->credit(credit.chunks);
  if (toward_b) {
    _client.note_path_progress(from.id, net::Lane::data);
    advance(credit.circuit, *toward_b);
  }
  return net::Handled::processed();
}

net::Handled Paths::relay_close(const NodeRecord& from, const net::Message& request) {
  RelayClose close;
  try {
    close = decode_relay_close(request.payload);
  } catch (const net::DecodeError& error) {
    return net::Handled::failed(400, "invalid_request", error.what());
  }
  std::optional<bool> toward_b;
  bool ours = false;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _ends.find(close.circuit); found != _ends.end() && found->second->relay() == from.id) {
      ours = true;
    } else if (const auto circuit = _circuits.find(close.circuit); circuit != _circuits.end()) {
      auto& c = circuit->second;
      if (from.id == c.a) toward_b = true;
      if (from.id == c.b) toward_b = false;
      if (toward_b && !(*toward_b ? c.to_b : c.to_a).close) {
        auto& leg = *toward_b ? c.to_b : c.to_a;
        leg.close = close;
        leg.close_since = std::chrono::steady_clock::now();
      }
    }
  }
  if (ours) close_end(close.circuit, close.why, false, close.chunks);
  if (toward_b) {
    advance(close.circuit, *toward_b);
    // Looked at again when the bound could run out, in case its last chunks never come.
    watch_close(close.circuit, *toward_b);
  }
  return net::Handled::processed();
}

std::optional<Observation> Paths::observe() {
  const auto self = _self();
  Observation out;
  for (const auto& peer : _roster()) {
    if (out.seen.size() == 2) break;
    if (peer.id == self.id || !peer.inbound_capable() || peer.host.empty() || !peer.port) continue;
    try {
      const auto [port, seen] = observe_from(_io, _config.credentials, self, out.local_port, {peer.host, peer.port}, peer.id, _config.connect_timeout);
      out.local_port = port;
      out.seen.push_back(seen);
    } catch (const std::exception&) {
      // Another peer may answer.
    }
  }
  if (out.seen.empty()) return std::nullopt;
  return out;
}

void Paths::sweep() {
  std::vector<std::pair<uint64_t, Circuit>> dead;
  {
    contract::Lock lock(_mutex);
    for (auto it = _circuits.begin(); it != _circuits.end();) {
      const bool a = _client.has_route(it->second.a, net::Lane::control);
      const bool b = _client.has_route(it->second.b, net::Lane::control);
      if (a && b) {
        ++it;
        continue;
      }
      dead.emplace_back(it->first, it->second);
      it = _circuits.erase(it);
    }
  }
  for (const auto& [circuit, c] : dead) {
    const RelayClose close{circuit, "the relay lost an end"};
    for (const auto& end : {c.a, c.b}) (void)_client.send(end, net::Lane::control, {m::relay_close, encode(close), {}}, sched::FrameType::control);
  }
  // Moving up: a relayed pair whose ends can now both be punched.
  const auto relayed = _client.routes_on(net::Purpose::relayed);
  if (relayed.empty()) return;
  const auto self = _self();
  if (self.path_class == membership::PathClass::relay_only) return;
  const auto now = std::chrono::steady_clock::now();
  std::vector<std::pair<NodeRecord, net::Lane>> due;
  for (const auto& [peer_id, lane] : relayed) {
    if (!(self.id < peer_id)) continue;
    const auto peer = known(peer_id);
    if (!peer || peer->path_class == membership::PathClass::relay_only) continue;
    contract::Lock lock(_mutex);
    if (_stopped) return;
    auto& after = _upgrade_after[{peer_id, lane}];
    if (now < after) continue;
    after = now + _config.upgrade_every;
    due.emplace_back(*peer, lane);
  }
  for (auto& [peer, lane] : due) {
    {
      contract::Lock lock(_mutex);
      if (_stopped) return;
      ++_stats.upgrades_tried;
      _queue.push_back([this, peer, lane] {
        if (try_punch(_self(), peer, lane).empty()) {
          contract::Lock counted(_mutex);
          ++_stats.upgrades_made;
        }
      });
    }
    _wake.notify_one();
  }
}

PathStats Paths::stats() const {
  contract::Lock lock(_mutex);
  auto out = _stats;
  out.ends = _ends.size();
  for (const auto& [circuit, c] : _circuits) out.circuits.push_back({circuit, c.a, c.b, c.lane, c.bytes, c.to_a.carrier, c.to_b.carrier});
  return out;
}

}  // namespace dannan::reach
