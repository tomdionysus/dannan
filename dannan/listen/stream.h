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
  using TlsStream = boost::asio::ssl::stream<Socket&>;

  ~Stream();
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  executor_type get_executor() noexcept { return _socket.get_executor(); }

  // Reads what has arrived, as a socket's async_read_some; plaintext or TLS alike.
  // The handler's executor, allocator and cancellation slot reach the read, so a
  // composed operation's per-operation cancel (Beast's) cancels it (Lir's review).
  template <typename MutableBuffers, typename Token>
  auto async_read_some(const MutableBuffers& buffers, Token&& token) {
    return boost::asio::async_initiate<Token, void(boost::system::error_code, std::size_t)>(
        [self = shared_from_this()](auto handler, const MutableBuffers& to) {
          auto executor = boost::asio::get_associated_executor(handler, self->get_executor());
          auto allocator = boost::asio::get_associated_allocator(handler);
          auto slot = boost::asio::get_associated_cancellation_slot(handler);
          ++self->_reads;
          auto done = boost::asio::bind_executor(
              executor,
              boost::asio::bind_allocator(allocator, boost::asio::bind_cancellation_slot(
                                                         slot, [self, handler = std::move(handler)](boost::system::error_code error, std::size_t read) mutable {
                                                           if (read > 0) self->note_read();
                                                           self->read_ended();
                                                           std::move(handler)(error, read);
                                                         })));
          if (self->_tls)
            self->_tls->async_read_some(to, std::move(done));
          else
            self->_socket.async_read_some(to, std::move(done));
        },
        token, buffers);
  }

  // Writes what it can, as a socket's async_write_some. Not with send() on one stream.
  template <typename ConstBuffers, typename Token>
  auto async_write_some(const ConstBuffers& buffers, Token&& token) {
    return boost::asio::async_initiate<Token, void(boost::system::error_code, std::size_t)>(
        [self = shared_from_this()](auto handler, const ConstBuffers& from) {
          if (self->_tls)
            self->_tls->async_write_some(from, std::move(handler));
          else
            self->_socket.async_write_some(from, std::move(handler));
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

  // How long the connection may go without a byte read before it is closed at once;
  // zero (the default): never. Set at any time.
  void set_idle(std::chrono::milliseconds idle);

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
  // The protocol ALPN chose; empty for none.
  const std::string& alpn() const noexcept { return _alpn; }
  // "tcp" or "tls".
  const char* transport() const noexcept { return _tls ? "tls" : "tcp"; }
  // The TLS session, for what the hosted system reads of it; null before TLS.
  SSL* native_tls() noexcept { return _tls ? _tls->native_handle() : nullptr; }
  // The listener's name.
  const std::string& listener() const noexcept;

 private:
  friend class StreamListener;
  struct Private {};

 public:
  // Made by its listener.
  Stream(Private, Socket socket, std::shared_ptr<ListenerShared> shared);

 private:
  // The handshake as the server, within the listener's handshake deadline.
  void handshake(std::function<void(std::string)> done);
  void note_read();
  void arm_idle();
  void write_next();
  // The clean end once nothing is queued or being written.
  void shut_down();
  // A hosted read has completed: the TLS shutdown waiting for it may begin.
  void read_ended();
  void tls_shutdown();
  void finish(bool clean);

  Socket _socket;
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
  // Hosted reads outstanding, and a TLS shutdown waiting for them to end: it reads
  // the peer's close_notify itself, and cannot share the stream with another read.
  size_t _reads = 0;
  bool _shutdown_waiting = false;
  std::vector<contract::UniqueFunction<void()>> _finished;
};

}  // namespace dannan::listen
