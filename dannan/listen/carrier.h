//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <cstddef>
#include <memory>

// What carries a Stream's bytes (TODO/2026-11-20-stack-plan.md, item 17): a socket, as every
// listener and connector has, or one end of an in-memory pipe, so a stream runs inside the
// simulation's world or a test with no socket at all. Stream, its TLS and every protocol over it
// run on either unchanged. A carrier models Asio's AsyncReadStream and AsyncWriteStream: each
// operation reads or writes at most the first non-empty buffer it is given, as read_some and
// write_some may, and completes on the handler's associated executor with its associated
// allocator and cancellation slot honoured. Used on one executor, as a socket is.
namespace dannan::listen {

class Carrier {
 public:
  using executor_type = boost::asio::any_io_executor;
  using Handler = boost::asio::any_completion_handler<void(boost::system::error_code, std::size_t)>;
  // A TLS stream layered over a carrier asks for its lowest layer.
  using lowest_layer_type = Carrier;

  virtual ~Carrier() = default;
  virtual executor_type get_executor() noexcept = 0;
  lowest_layer_type& lowest_layer() noexcept { return *this; }

  template <typename MutableBuffers, typename Token>
  auto async_read_some(const MutableBuffers& buffers, Token&& token) {
    return boost::asio::async_initiate<Token, void(boost::system::error_code, std::size_t)>(
        [this](auto handler, boost::asio::mutable_buffer to) { read_some(to, Handler(std::move(handler))); }, token, first(buffers));
  }
  template <typename ConstBuffers, typename Token>
  auto async_write_some(const ConstBuffers& buffers, Token&& token) {
    return boost::asio::async_initiate<Token, void(boost::system::error_code, std::size_t)>(
        [this](auto handler, boost::asio::const_buffer from) { write_some(from, Handler(std::move(handler))); }, token, first_const(buffers));
  }

  virtual void read_some(boost::asio::mutable_buffer into, Handler done) = 0;
  virtual void write_some(boost::asio::const_buffer from, Handler done) = 0;
  // Bytes that a read would take at once.
  virtual std::size_t available(boost::system::error_code& error) const = 0;
  // No more writes from this end: the other reads its end of the stream.
  virtual void shutdown_send(boost::system::error_code& error) = 0;
  virtual void close(boost::system::error_code& error) = 0;
  // Operations outstanding end as aborted.
  virtual void cancel(boost::system::error_code& error) = 0;
  virtual bool is_open() const noexcept = 0;
  virtual void set_no_delay(bool on, boost::system::error_code& error) = 0;
  virtual bool no_delay(boost::system::error_code& error) const = 0;
  virtual boost::asio::ip::tcp::endpoint remote_endpoint(boost::system::error_code& error) const = 0;
  // "tcp" or "memory".
  virtual const char* transport() const noexcept = 0;

 private:
  template <typename Buffers>
  static boost::asio::mutable_buffer first(const Buffers& buffers) {
    for (auto it = boost::asio::buffer_sequence_begin(buffers); it != boost::asio::buffer_sequence_end(buffers); ++it) {
      const boost::asio::mutable_buffer buffer(*it);
      if (buffer.size()) return buffer;
    }
    return {};
  }
  template <typename Buffers>
  static boost::asio::const_buffer first_const(const Buffers& buffers) {
    for (auto it = boost::asio::buffer_sequence_begin(buffers); it != boost::asio::buffer_sequence_end(buffers); ++it) {
      const boost::asio::const_buffer buffer(*it);
      if (buffer.size()) return buffer;
    }
    return {};
  }
};

// A TCP socket as a carrier: what every listener and connector makes.
std::unique_ptr<Carrier> socket_carrier(boost::asio::ip::tcp::socket socket);

}  // namespace dannan::listen
