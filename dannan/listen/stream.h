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
#include <boost/asio/associated_allocator.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/contract/unique_function.h"
#include "dannan/listen/carrier.h"

namespace dannan::listen {

// A client certificate the peer presented, whatever its chain: the hosted system
// judges it. `verified` says whether its chain checked out against the listener's
// client bundle, and `reason` why not when it did not.
struct ClientCertificate {
  // SHA-256 over the certificate's DER: RFC 8705's x5t#S256.
  std::array<uint8_t, 32> thumbprint{};
  // RFC 2253.
  std::string subject;
  bool verified = false;
  std::string reason;
};

struct ListenerShared;
namespace detail {
struct ServerTls;
}

// One accepted connection, plaintext or TLS (P3). It models Asio's AsyncReadStream
// and AsyncWriteStream, so a protocol's parser (Beast's HTTP and WebSocket, an MQTT
// or LDAP reader) runs over it as over any socket; it lives on a strand of the
// executor its listener was started with, and like a socket is used there: every
// member but send(), close(), abort() and the readers of what was negotiated is
// called on its executor. send(), close() and abort() may be called from anywhere.
class Stream : public std::enable_shared_from_this<Stream> {
 public:
  using executor_type = boost::asio::any_io_executor;
  using Socket = boost::asio::ip::tcp::socket;
  // TLS is layered over the carrier, whatever carries the bytes.
  using TlsStream = boost::asio::ssl::stream<Carrier&>;

  ~Stream();
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  executor_type get_executor() noexcept { return _carrier->get_executor(); }

  // Reads what has arrived, as a socket's async_read_some; plaintext or TLS alike.
  // The handler's executor, allocator and cancellation slot reach the read, so a
  // composed operation's per-operation cancel (Beast's) cancels it (Lir's review).
  // Once the stream's close has begun it ends at once as aborted.
  template <typename MutableBuffers, typename Token>
  auto async_read_some(const MutableBuffers& buffers, Token&& token) {
    return boost::asio::async_initiate<Token, void(boost::system::error_code, std::size_t)>(
        [self = shared_from_this()](auto handler, const MutableBuffers& to) {
          auto done = self->tracked(std::move(handler), true);
          if (!done) return;
          if (self->_tls)
            self->_tls->async_read_some(to, std::move(*done));
          else
            self->_carrier->async_read_some(to, std::move(*done));
        },
        token, buffers);
  }

  // Writes what it can, as a socket's async_write_some. Not with send() on one stream.
  // Once the stream's close has begun it ends at once as aborted.
  template <typename ConstBuffers, typename Token>
  auto async_write_some(const ConstBuffers& buffers, Token&& token) {
    return boost::asio::async_initiate<Token, void(boost::system::error_code, std::size_t)>(
        [self = shared_from_this()](auto handler, const ConstBuffers& from) {
          auto done = self->tracked(std::move(handler), false);
          if (!done) return;
          if (self->_tls)
            self->_tls->async_write_some(from, std::move(*done));
          else
            self->_carrier->async_write_some(from, std::move(*done));
        },
        token, buffers);
  }

  // Queues `bytes` to go after what is queued, never waiting: a sender may send one
  // write per packet without waiting for the last. Once more than the listener's
  // outbound bound is queued, the reader is too slow: the stream is closed at once.
  // Not with async_write_some on one stream. From any thread; on the stream's
  // executor it is queued before it returns, so a close() after it sends it first.
  // After close() it is dropped.
  void send(std::string bytes);
  // Bytes queued by send() and not yet written.
  size_t queued() const noexcept { return _queued.load(); }

  // Upgrades a plaintext connection to TLS in place (StartTLS), once the hosted system
  // has read the request: checks that no byte has arrived past it (plaintext where
  // none may be ends the connection), writes the hosted system's plaintext `answer`
  // (LDAP's ExtendedResponse), then takes the handshake. The answer is Dannan's to
  // send, so the check is made before a client could begin its handshake. `done` is
  // told on the stream's executor, empty when the handshake completed. Only on a
  // listener with certificates, and only once. Refused while bytes queued by send()
  // are not yet written (Lugh's review): they would go out after the answer.
  void start_tls(std::string answer, std::function<void(std::string)> done);
  // StartTLS where the protocol sends no answer and the client begins its handshake at
  // once (MySQL's SSLRequest, then its ClientHello with nothing between: Kvasir's ask).
  // As start_tls, but nothing is written, and bytes already waiting are the client's
  // handshake, so they are not refused. The hosted system must have read exactly the
  // request: whatever it read past it is lost to the handshake. Once, only with
  // certificates, refused while bytes queued by send() are not yet written; `done` as
  // start_tls's.
  void start_tls_unanswered(std::function<void(std::string)> done);

  // How long the connection may go without a byte read before it is closed at once;
  // zero (the default): never. Set at any time.
  void set_idle(std::chrono::milliseconds idle);

  // Whether a write goes out at once (TCP_NODELAY), as every accepted socket does: a
  // public protocol's small packets must not wait on Nagle behind the peer's delayed
  // ACK (Lir: about 40 ms on an MQTT PUBACK). False turns Nagle back on, for a
  // protocol that writes many tiny pieces of one message. From any thread; applied
  // on the stream's executor.
  void set_no_delay(bool on);
  // Whether it is set; false once the socket is closed. On the stream's executor
  // only, as every reader of the socket (Lir's review: off it, a read racing a close
  // could meet a reused descriptor).
  bool no_delay() const;

  // Ends the connection cleanly: what send() queued is written first, then TLS's
  // close_notify, then the socket, all within the listener's close bound (Lir's
  // review: its DISCONNECT goes by send() and must not be lost). A read still pending
  // when the close begins ends as aborted. From any thread; once.
  void close();
  // Ends it at once.
  void abort();
  // `done` is called on the stream's executor once it has finished, closed either
  // way; at once (posted) if it already has.
  void when_finished(contract::UniqueFunction<void()> done);

  bool is_open() const noexcept { return !_closed.load(); }
  bool is_tls() const noexcept { return _tls != nullptr; }
  const boost::asio::ip::tcp::endpoint& peer() const noexcept { return _peer; }
  // The client certificate presented, if any; set once the handshake completed.
  const std::optional<ClientCertificate>& client_certificate() const noexcept { return _client_certificate; }
  // The other end's certificate at either end: an accepted stream's client
  // certificate, or a connected stream's server certificate, verified (Lugh's review:
  // client_certificate() would be misread on an outbound stream).
  const std::optional<ClientCertificate>& peer_certificate() const noexcept { return _client_certificate; }
  // The protocol ALPN chose; empty for none.
  const std::string& alpn() const noexcept { return _alpn; }
  // "tls", or the carrier's: "tcp", or "memory" for one inside a test.
  const char* transport() const noexcept { return _tls ? "tls" : _carrier->transport(); }
  // The TLS session, for what the hosted system reads of it; null before TLS.
  SSL* native_tls() noexcept { return _tls ? _tls->native_handle() : nullptr; }
  // The listener's name.
  const std::string& listener() const noexcept;

 private:
  friend class StreamListener;
  friend class StreamConnector;
  struct Private {};

 public:
  // Made by its listener or connector, over a socket or another carrier.
  Stream(Private, Socket socket, std::shared_ptr<ListenerShared> shared);
  Stream(Private, std::unique_ptr<Carrier> carrier, std::shared_ptr<ListenerShared> shared);

 private:
  // The handshake as the server, within the listener's handshake deadline.
  void handshake(std::function<void(std::string)> done);
  // The handshake as the client, under `context`, checking the server's certificate
  // against `name` (a DNS name, or an IP literal with no SNI). On the stream's executor;
  // the connector holds its deadline.
  void client_handshake(std::shared_ptr<detail::ServerTls> context, const std::string& name, std::function<void(std::string)> done);
  void note_read();
  void arm_idle();
  void write_next();
  // A hosted operation's completion, counted while it is outstanding, so the TLS
  // shutdown, which reads and writes the stream itself, never shares it with one.
  // None once the close has begun: the handler is then told aborted, posted.
  template <typename Handler>
  auto tracked(Handler handler, bool reading) {
    auto executor = boost::asio::get_associated_executor(handler, get_executor());
    auto allocator = boost::asio::get_associated_allocator(handler);
    auto slot = boost::asio::get_associated_cancellation_slot(handler);
    using Done = decltype(boost::asio::bind_executor(
        executor, boost::asio::bind_allocator(allocator, boost::asio::bind_cancellation_slot(slot, Completion<Handler>{nullptr, std::move(handler), false}))));
    if (_shutting_down) {
      boost::asio::post(executor, [handler = std::move(handler)]() mutable { std::move(handler)(boost::asio::error::operation_aborted, std::size_t{0}); });
      return std::optional<Done>{};
    }
    ++_ops;
    return std::optional<Done>(boost::asio::bind_executor(
        executor, boost::asio::bind_allocator(
                      allocator, boost::asio::bind_cancellation_slot(slot, Completion<Handler>{shared_from_this(), std::move(handler), reading}))));
  }
  template <typename Handler>
  struct Completion {
    std::shared_ptr<Stream> self;
    Handler handler;
    bool reading;
    void operator()(boost::system::error_code error, std::size_t bytes) {
      if (reading && bytes > 0) self->note_read();
      self->op_ended();
      std::move(handler)(error, bytes);
    }
  };

  // The clean end once nothing is queued or being written.
  void shut_down();
  // A hosted operation has completed: the TLS shutdown waiting for it may begin.
  void op_ended();
  void tls_shutdown();
  void finish(bool clean);

  std::unique_ptr<Carrier> _carrier;
  std::shared_ptr<ListenerShared> _shared;
  boost::asio::ip::tcp::endpoint _peer;
  std::unique_ptr<TlsStream> _tls;
  std::shared_ptr<detail::ServerTls> _context;
  std::optional<ClientCertificate> _client_certificate;
  std::string _alpn;
  bool _tls_started = false;

  // On the stream's executor.
  std::deque<std::string> _outbound;
  bool _writing = false;
  std::atomic<size_t> _queued{0};
  std::atomic<bool> _slow{false};

  boost::asio::steady_timer _idle_timer;
  std::chrono::milliseconds _idle{0};
  std::atomic<contract::Clock::rep> _last_read{0};
  std::atomic<bool> _closed{false};
  // On the stream's executor.
  bool _closing = false;
  bool _shutting_down = false;
  // Hosted reads and writes outstanding, and a TLS shutdown waiting for them to end:
  // it reads and writes the stream itself, and cannot share it with another operation.
  size_t _ops = 0;
  bool _shutdown_waiting = false;
  std::vector<contract::UniqueFunction<void()>> _finished;
};

}  // namespace dannan::listen
