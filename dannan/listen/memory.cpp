//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/memory.h"

#include <algorithm>
#include <boost/asio/append.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/post.hpp>
#include <cstring>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"

namespace dannan::listen {
namespace {

namespace asio = boost::asio;
using error_code = boost::system::error_code;
using Handler = Carrier::Handler;

// A completion to make once the lock is let go: posted to its handler's executor.
struct Due {
  Handler handler;
  asio::any_io_executor fallback;
  error_code error;
  std::size_t bytes = 0;
};

void complete(std::vector<Due>& due) {
  // Posted with its arguments appended: post runs it through the handler's own associated
  // executor (a strand stays a strand), and never inline.
  for (auto& d : due) asio::post(d.fallback, asio::append(std::move(d.handler), d.error, d.bytes));
  due.clear();
}

size_t index(End end) { return end == End::client ? 0 : 1; }
End other(End end) { return end == End::client ? End::server : End::client; }

class Pipe final : public MemoryLink, public std::enable_shared_from_this<Pipe> {
 public:
  Pipe(asio::any_io_executor client, asio::any_io_executor server, MemoryOptions options) : _options(std::move(options)) {
    _ends[0].executor = std::move(client);
    _ends[1].executor = std::move(server);
  }

  // --- The test's hand ----------------------------------------------------
  void sever() override {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      _severed = true;
      for (auto& end : _ends) settle_all_locked(end, asio::error::connection_reset, due);
    }
    complete(due);
  }
  void stall() override {
    contract::Lock lock(_mutex);
    _stalled = true;
  }
  void resume() override {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      _stalled = false;
      for (const auto end : {End::client, End::server}) flow_locked(end, due);
    }
    complete(due);
  }
  void set_pieces(End from, std::function<size_t(size_t)> piece) override {
    contract::Lock lock(_mutex);
    _ends[index(from)].pieces = std::move(piece);
  }
  bool severed() const override {
    contract::Lock lock(_mutex);
    return _severed;
  }
  bool stalled() const override {
    contract::Lock lock(_mutex);
    return _stalled;
  }

  // --- One end's carrier --------------------------------------------------
  asio::any_io_executor executor(End at) const {
    contract::Lock lock(_mutex);
    return _ends[index(at)].executor;
  }
  const asio::ip::tcp::endpoint& address_of(End at) const { return at == End::client ? _options.client_address : _options.server_address; }

  void read(End at, asio::mutable_buffer into, Handler done) {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      auto& end = _ends[index(at)];
      if (end.closed) {
        due.push_back({std::move(done), end.executor, asio::error::bad_descriptor, 0});
      } else if (_severed) {
        due.push_back({std::move(done), end.executor, asio::error::connection_reset, 0});
      } else if (into.size() == 0) {
        due.push_back({std::move(done), end.executor, {}, 0});
      } else {
        auto slot = asio::get_associated_cancellation_slot(done);
        const auto id = ++end.reads;
        end.reading = Reading{into, std::move(done), id};
        if (slot.is_connected())
          slot.assign([weak = weak_from_this(), at, id](asio::cancellation_type) {
            if (const auto pipe = weak.lock()) pipe->abort_read(at, id);
          });
        flow_locked(at, due);
      }
    }
    complete(due);
  }

  void write(End at, asio::const_buffer from, Handler done) {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      auto& end = _ends[index(at)];
      const auto& peer = _ends[index(other(at))];
      if (end.closed) {
        due.push_back({std::move(done), end.executor, asio::error::bad_descriptor, 0});
      } else if (_severed) {
        due.push_back({std::move(done), end.executor, asio::error::connection_reset, 0});
      } else if (end.shut || peer.closed) {
        due.push_back({std::move(done), end.executor, asio::error::broken_pipe, 0});
      } else if (from.size() == 0) {
        due.push_back({std::move(done), end.executor, {}, 0});
      } else {
        end.writing = Writing{from, std::move(done)};
        take_write_locked(at, due);
        flow_locked(other(at), due);
      }
    }
    complete(due);
  }

  size_t available(End at) const {
    contract::Lock lock(_mutex);
    if (_stalled || _severed) return 0;
    return _ends[index(other(at))].sent.size();
  }

  void shutdown_send(End at) {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      _ends[index(at)].shut = true;
      flow_locked(other(at), due);
    }
    complete(due);
  }

  void close(End at) {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      auto& end = _ends[index(at)];
      if (end.closed) return;
      end.closed = true;
      end.shut = true;
      settle_all_locked(end, asio::error::operation_aborted, due);
      // The peer reads what is left and then the end; a write it has waiting fails.
      flow_locked(other(at), due);
      auto& peer = _ends[index(other(at))];
      if (peer.writing) {
        due.push_back({std::move(peer.writing->done), peer.executor, asio::error::broken_pipe, 0});
        peer.writing.reset();
      }
    }
    complete(due);
  }

  void cancel(End at) {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      settle_all_locked(_ends[index(at)], asio::error::operation_aborted, due);
    }
    complete(due);
  }

  bool is_open(End at) const {
    contract::Lock lock(_mutex);
    return !_ends[index(at)].closed;
  }

  void set_no_delay(End at, bool on) {
    contract::Lock lock(_mutex);
    _ends[index(at)].no_delay = on;
  }
  bool no_delay(End at) const {
    contract::Lock lock(_mutex);
    return _ends[index(at)].no_delay;
  }

 private:
  struct Reading {
    asio::mutable_buffer into;
    Handler done;
    // Which read of this end it is: a cancellation fires only the read it was bound to.
    uint64_t id = 0;
  };
  struct Writing {
    asio::const_buffer from;
    Handler done;
  };
  struct EndState {
    asio::any_io_executor executor;
    // What this end wrote and the other has not yet read.
    std::deque<uint8_t> sent;
    std::optional<Reading> reading;
    std::optional<Writing> writing;
    std::function<size_t(size_t)> pieces;
    // Reads started at this end, the last one's id.
    uint64_t reads = 0;
    // No more writes from this end (a half-close, or a close).
    bool shut = false;
    bool closed = false;
    bool no_delay = true;
  };

  // A parked read's handler, its cancellation slot cleared: a signal after this must not reach a
  // later read.
  static Handler taken(std::optional<Reading>& reading) {
    auto done = std::move(reading->done);
    reading.reset();
    asio::get_associated_cancellation_slot(done).clear();
    return done;
  }

  // From the read's cancellation slot, inside its handler: the slot is not cleared here, since that
  // would destroy the handler running (the bus error this once caused). The id keeps a signal
  // bound to an earlier read from reaching a later one.
  void abort_read(End at, uint64_t id) {
    std::vector<Due> due;
    {
      contract::Lock lock(_mutex);
      auto& end = _ends[index(at)];
      if (end.reading && end.reading->id == id) {
        due.push_back({std::move(end.reading->done), end.executor, asio::error::operation_aborted, 0});
        end.reading.reset();
      }
    }
    complete(due);
  }

  void settle_all_locked(EndState& end, error_code error, std::vector<Due>& due) DANNAN_REQUIRES(_mutex) {
    if (end.reading) due.push_back({taken(end.reading), end.executor, error, 0});
    if (end.writing) {
      due.push_back({std::move(end.writing->done), end.executor, error, 0});
      end.writing.reset();
    }
  }

  // As much of `at`'s waiting write as there is room for.
  void take_write_locked(End at, std::vector<Due>& due) DANNAN_REQUIRES(_mutex) {
    auto& end = _ends[index(at)];
    if (!end.writing) return;
    const auto room = _options.capacity > end.sent.size() ? _options.capacity - end.sent.size() : 0;
    if (room == 0) return;
    const auto n = std::min(room, end.writing->from.size());
    const auto* bytes = static_cast<const uint8_t*>(end.writing->from.data());
    end.sent.insert(end.sent.end(), bytes, bytes + n);
    due.push_back({std::move(end.writing->done), end.executor, {}, n});
    end.writing.reset();
  }

  // Delivers to `to`'s waiting read what the other end sent, or the end of the stream, unless
  // stalled; then lets the other end's waiting write into the room made.
  void flow_locked(End to, std::vector<Due>& due) DANNAN_REQUIRES(_mutex) {
    auto& reader = _ends[index(to)];
    auto& writer = _ends[index(other(to))];
    if (!reader.reading || _stalled) return;
    if (!writer.sent.empty()) {
      auto n = std::min(reader.reading->into.size(), writer.sent.size());
      if (writer.pieces) n = std::max<size_t>(1, std::min(n, writer.pieces(writer.sent.size())));
      auto* into = static_cast<uint8_t*>(reader.reading->into.data());
      std::copy_n(writer.sent.begin(), n, into);
      writer.sent.erase(writer.sent.begin(), writer.sent.begin() + static_cast<std::ptrdiff_t>(n));
      due.push_back({taken(reader.reading), reader.executor, {}, n});
      take_write_locked(other(to), due);
      return;
    }
    if (writer.shut) {
      due.push_back({taken(reader.reading), reader.executor, asio::error::eof, 0});
    }
  }

  const MemoryOptions _options;
  mutable contract::Mutex _mutex;
  EndState _ends[2] DANNAN_GUARDED_BY(_mutex);
  bool _severed DANNAN_GUARDED_BY(_mutex) = false;
  bool _stalled DANNAN_GUARDED_BY(_mutex) = false;
};

class MemoryCarrier final : public Carrier {
 public:
  MemoryCarrier(std::shared_ptr<Pipe> pipe, End at) : _pipe(std::move(pipe)), _at(at) {}
  ~MemoryCarrier() override {
    error_code ignored;
    close(ignored);
  }

  executor_type get_executor() noexcept override { return _pipe->executor(_at); }
  void read_some(asio::mutable_buffer into, Handler done) override { _pipe->read(_at, into, std::move(done)); }
  void write_some(asio::const_buffer from, Handler done) override { _pipe->write(_at, from, std::move(done)); }
  std::size_t available(error_code& error) const override {
    error = {};
    return _pipe->available(_at);
  }
  void shutdown_send(error_code& error) override {
    error = {};
    _pipe->shutdown_send(_at);
  }
  void close(error_code& error) override {
    error = {};
    _pipe->close(_at);
  }
  void cancel(error_code& error) override {
    error = {};
    _pipe->cancel(_at);
  }
  bool is_open() const noexcept override { return _pipe->is_open(_at); }
  void set_no_delay(bool on, error_code& error) override {
    error = {};
    _pipe->set_no_delay(_at, on);
  }
  bool no_delay(error_code& error) const override {
    error = {};
    return _pipe->no_delay(_at);
  }
  asio::ip::tcp::endpoint remote_endpoint(error_code& error) const override {
    error = {};
    return _pipe->address_of(other(_at));
  }
  const char* transport() const noexcept override { return "memory"; }

 private:
  std::shared_ptr<Pipe> _pipe;
  End _at;
};

}  // namespace

MemoryPair memory_pair(asio::any_io_executor client, asio::any_io_executor server, MemoryOptions options) {
  auto pipe = std::make_shared<Pipe>(std::move(client), std::move(server), std::move(options));
  MemoryPair out;
  out.client = std::make_unique<MemoryCarrier>(pipe, End::client);
  out.server = std::make_unique<MemoryCarrier>(pipe, End::server);
  out.link = pipe;
  return out;
}

}  // namespace dannan::listen
