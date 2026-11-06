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
#include <boost/asio/connect.hpp>
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
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
  writer.bytes(value.bytes);
  return writer.take();
}

net::Bytes encode(const RelayClose& value) {
  net::Writer writer;
  writer.u64(value.circuit);
  writer.string(value.why);
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
    out.bytes = reader.bytes(net::max_frame_size);
    return out;
  });
}

RelayClose decode_relay_close(std::span<const uint8_t> bytes) {
  return decoding<RelayClose>(bytes, [](net::Reader& reader) {
    RelayClose out;
    out.circuit = reader.u64();
    out.why = reader.string(4096);
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
  asio::dispatch(state->strand, [state, remote, timeout, key, local] {
    state->timer.expires_after(timeout);
    state->timer.async_wait([state](const boost::system::error_code& error) {
      if (!error) state->fail("timed out");
    });
    state->tcp.async_connect(remote, [state, key, local](const boost::system::error_code& error) {
      if (state->finished) return;
      if (error) return state->fail("connect: " + error.message());
      const auto family = state->tcp.local_endpoint().protocol().family();
      state->socket.emplace(state->strand, net::Session::Socket::protocol_type(family, SOCK_STREAM), state->tcp.release());
      net::async_dial_handshake(*state->socket, key, local, net::min_frame_size, net::Lane::probe,
                                [state](std::exception_ptr error, net::Established established) {
                                  if (state->finished) return;
                                  if (error) {
                                    try {
                                      std::rethrow_exception(error);
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
    bool connecting = false;

    Opening(asio::io_context& io, asio::ip::tcp::endpoint remote, uint16_t port, std::chrono::milliseconds within,
            std::function<void(int, int, std::string)> done)
        : strand(asio::make_strand(io.get_executor())),
          tcp(strand),
          timer(strand),
          remote(remote),
          port(port),
          until(std::chrono::steady_clock::now() + within),
          done(std::move(done)) {}

    // On the strand: one attempt at a time, each bound to the punch port.
    void attempt() {
      if (std::chrono::steady_clock::now() >= until) return done(-1, 0, "the simultaneous open did not complete: " + last_error);
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
      timer.expires_after(std::chrono::milliseconds(700));
      timer.async_wait([self = shared_from_this()](const boost::system::error_code& error) {
        if (error || !self->connecting) return;
        boost::system::error_code ignored;
        self->tcp.close(ignored);
      });
      tcp.async_connect(remote, [self = shared_from_this()](const boost::system::error_code& error) {
        self->connecting = false;
        self->timer.cancel();
        if (!error) {
          // The warning, if any, comes from the node's first ordinary socket.
          net::peer_socket_options(self->tcp.native_handle(), status::ScopedLogger(status::null_sink(), "net"));
          const int family = self->tcp.local_endpoint().protocol().family();
          return self->done(self->tcp.release(), family, "");
        }
        self->last_error = error == asio::error::operation_aborted ? "timed out" : error.message();
        boost::system::error_code ignored;
        self->tcp.close(ignored);
        // A refusal is the far end's kernel answering before its own connect has begun:
        // on a path with nothing to drop the SYN, the two ends meet only if their
        // attempts overlap, so try again almost at once.
        self->again(std::chrono::milliseconds(error == asio::error::connection_refused ? 5 : 100));
      });
    }
    void again(std::chrono::milliseconds after) {
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
       int descriptor)
      : _strand(asio::make_strand(io.get_executor())),
        _socket(_strand, net::Session::Socket::protocol_type(AF_UNIX, SOCK_STREAM), descriptor),
        _owner(std::move(owner)),
        _circuit(circuit),
        _relay(relay),
        _far_end(far_end),
        _lane(lane),
        _frame(lane == net::Lane::data ? sched::FrameType::loader : sched::FrameType::control) {}

  const identity::NodeId& relay() const noexcept { return _relay; }

  void start() {
    asio::dispatch(_strand, [self = shared_from_this()] { self->read(); });
  }
  void deliver(net::Bytes bytes) {
    asio::post(_strand, [self = shared_from_this(), bytes = std::move(bytes)]() mutable {
      if (self->_closed) return;
      self->far_end_moved();
      self->_out.push_back(std::move(bytes));
      if (self->_out.size() == 1) self->write();
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
  // The far end has finished: its last bytes reach the session first.
  void finish() {
    asio::post(_strand, [self = shared_from_this()] {
      self->_finishing = true;
      if (self->_out.empty()) self->close();
    });
  }

 private:
  void far_end_moved() {
    Paths::with(_owner, [&](Paths& paths) { paths._client.note_path_progress(_far_end, _lane); });
  }
  // One read at a time, and none past the window or with a chunk still
  // waiting to go: the session writing into the far side of the socket feels
  // a slow circuit as a slow socket.
  void read() {
    if (_closed || _reading || _carrying || _unanswered >= relay_window_chunks) return;
    _reading = true;
    _socket.async_read_some(asio::buffer(_buffer), [self = shared_from_this()](const boost::system::error_code& error, size_t got) {
      self->_reading = false;
      if (error) return self->ended(self->_closed ? "closed" : "the session's end closed: " + error.message());
      ++self->_unanswered;
      self->_carrying = net::Bytes(self->_buffer.begin(), self->_buffer.begin() + static_cast<std::ptrdiff_t>(got));
      self->carry();
    });
  }
  void carry() {
    if (_closed) return;
    auto outcome = net::Client::Paced::gone;
    Paths::with(_owner, [&](Paths& paths) {
      outcome = paths._client.send_paced(_relay, net::Lane::control, {m::relay_data, encode(RelayData{_circuit, *_carrying}), {}}, _frame, _strand,
                                         [self = shared_from_this()](bool written) {
                                           if (!written) return self->ended("the relay's session ended");
                                           self->_carrying.reset();
                                           self->read();
                                         });
    });
    if (outcome == net::Client::Paced::gone) return ended("the relay could not take the circuit's bytes");
    if (outcome == net::Client::Paced::full) later([](Pump& pump) { pump.carry(); });
  }
  void write() {
    asio::async_write(_socket, asio::buffer(_out.front()), [self = shared_from_this()](const boost::system::error_code& error, size_t) {
      if (error) return self->ended("the session's end closed: " + error.message());
      self->_out.pop_front();
      ++self->_owed;
      self->pay();
      if (!self->_out.empty()) return self->write();
      if (self->_finishing) self->close();
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
  void ended(const std::string& why) {
    if (_ended) return;
    _ended = true;
    _closed = true;
    boost::system::error_code ignored;
    _socket.close(ignored);
    Paths::with(_owner, [&](Paths& paths) { paths.close_end(_circuit, why, true); });
  }

  asio::strand<asio::any_io_executor> _strand;
  net::Session::Socket _socket;
  const std::shared_ptr<Paths::Owner> _owner;
  const uint64_t _circuit;
  const identity::NodeId _relay;
  const identity::NodeId _far_end;
  const net::Lane _lane;
  const sched::FrameType _frame;
  std::array<uint8_t, relay_chunk_bytes> _buffer{};
  std::deque<net::Bytes> _out;
  // The chunk read and not yet written to the relay's session.
  std::optional<net::Bytes> _carrying;
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
                        [opened](int descriptor, int family, std::string why) { opened->set_value({descriptor, family, std::move(why)}); });
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
  auto pump = std::make_shared<Pump>(_io, _owner, circuit, relay, far_end, lane, pair[1]);
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

void Paths::close_end(uint64_t circuit, const std::string& why, bool tell_relay) {
  std::shared_ptr<Pump> pump;
  {
    contract::Lock lock(_mutex);
    if (const auto found = _ends.find(circuit); found != _ends.end()) {
      pump = found->second;
      _ends.erase(found);
    }
  }
  if (!pump) return;
  if (!tell_relay) return pump->finish();
  pump->close();
  send_until_taken(pump->relay(), {m::relay_close, encode(RelayClose{circuit, why}), {}});
}

void Paths::shortly(std::function<void(Paths&)> f) {
  auto timer = std::make_shared<asio::steady_timer>(_io, std::chrono::milliseconds(5));
  timer->async_wait([timer, owner = _owner, f = std::move(f)](const boost::system::error_code& error) {
    if (!error) with(owner, f);
  });
}

void Paths::send_until_taken(identity::NodeId peer, net::Message message) {
  const auto outcome = _client.send_paced(peer, net::Lane::control, message, sched::FrameType::control, _io.get_executor(), {});
  if (outcome == net::Client::Paced::full) shortly([peer, message = std::move(message)](Paths& paths) { paths.send_until_taken(peer, message); });
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
        with(owner, [](Paths& paths) {
          contract::Lock lock(paths._mutex);
          ++paths._stats.punches_made;
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
  {
    contract::Lock lock(_mutex);
    ++_stats.circuits_relayed;
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
        if (!broke_window && !leg.lost) leg.held.push_back(std::move(data.bytes));
      }
    }
  }
  if (end) {
    end->deliver(std::move(data.bytes));
    return net::Handled::processed();
  }
  if (broke_window) {
    lost(data.circuit, *toward_b);
    return net::Handled::processed();
  }
  if (toward_b) advance(data.circuit, *toward_b);
  return net::Handled::processed();
}

void Paths::advance(uint64_t circuit, bool toward_b) {
  net::Message message;
  identity::NodeId peer;
  auto frame = sched::FrameType::control;
  // Taken off the leg before the send: its completion may run on another
  // thread before the send returns. Put back only if the queue refuses it.
  uint64_t credit = 0;
  std::optional<net::Bytes> chunk;
  bool last = false;
  {
    contract::Lock lock(_mutex);
    const auto found = _circuits.find(circuit);
    if (found == _circuits.end()) return;
    auto& c = found->second;
    auto& leg = toward_b ? c.to_b : c.to_a;
    if (leg.sending || leg.lost) return;
    peer = toward_b ? c.b : c.a;
    if (leg.credit) {
      credit = std::exchange(leg.credit, 0);
      message = {m::relay_credit, encode(RelayCredit{circuit, credit}), {}};
    } else if (!leg.held.empty()) {
      chunk = std::move(leg.held.front());
      leg.held.pop_front();
      frame = c.lane == net::Lane::data ? sched::FrameType::loader : sched::FrameType::control;
      message = {m::relay_data, encode(RelayData{circuit, *chunk}), {}};
    } else if (leg.close) {
      last = true;
      message = {m::relay_close, encode(RelayClose{circuit, *leg.close}), {}};
    } else {
      return;
    }
    leg.sending = true;
  }
  const auto outcome = _client.send_paced(
      peer, net::Lane::control, std::move(message), frame, _io.get_executor(),
      [owner = _owner, circuit, toward_b, last](bool written) { with(owner, [&](Paths& paths) { paths.advanced(circuit, toward_b, written, last); }); });
  if (outcome == net::Client::Paced::taken) return;
  {
    contract::Lock lock(_mutex);
    const auto found = _circuits.find(circuit);
    if (found == _circuits.end()) return;
    auto& leg = toward_b ? found->second.to_b : found->second.to_a;
    leg.sending = false;
    leg.credit += credit;
    if (chunk) leg.held.push_front(std::move(*chunk));
  }
  if (outcome == net::Client::Paced::gone) return lost(circuit, toward_b);
  shortly([circuit, toward_b](Paths& paths) { paths.advance(circuit, toward_b); });
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
    (toward_b ? found->second.to_b : found->second.to_a).sending = false;
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
    leg.lost = true;
    leg.held.clear();
    leg.credit = 0;
    if (other.lost) {
      _circuits.erase(found);
      return;
    }
    if (!other.close) other.close = "the relay lost the other end";
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
  if (toward_b) advance(credit.circuit, *toward_b);
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
      if (toward_b && !(*toward_b ? c.to_b : c.to_a).close) (*toward_b ? c.to_b : c.to_a).close = close.why;
    }
  }
  if (ours) close_end(close.circuit, close.why, false);
  if (toward_b) advance(close.circuit, *toward_b);
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
          contract::Lock lock(_mutex);
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
  for (const auto& [circuit, c] : _circuits) out.circuits.push_back({circuit, c.a, c.b, c.lane, c.bytes});
  return out;
}

}  // namespace dannan::reach
