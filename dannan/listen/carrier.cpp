//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/carrier.h"

namespace dannan::listen {
namespace {

using tcp = boost::asio::ip::tcp;

class SocketCarrier final : public Carrier {
 public:
  explicit SocketCarrier(tcp::socket socket) : _socket(std::move(socket)) {}

  executor_type get_executor() noexcept override { return _socket.get_executor(); }
  void read_some(boost::asio::mutable_buffer into, Handler done) override { _socket.async_read_some(into, std::move(done)); }
  void write_some(boost::asio::const_buffer from, Handler done) override { _socket.async_write_some(from, std::move(done)); }
  std::size_t available(boost::system::error_code& error) const override { return _socket.available(error); }
  void shutdown_send(boost::system::error_code& error) override { _socket.shutdown(tcp::socket::shutdown_send, error); }
  void close(boost::system::error_code& error) override { _socket.close(error); }
  void cancel(boost::system::error_code& error) override { _socket.cancel(error); }
  bool is_open() const noexcept override { return _socket.is_open(); }
  void set_no_delay(bool on, boost::system::error_code& error) override {
    if (_socket.is_open()) _socket.set_option(tcp::no_delay(on), error);
  }
  bool no_delay(boost::system::error_code& error) const override {
    tcp::no_delay option;
    _socket.get_option(option, error);
    return !error && option.value();
  }
  tcp::endpoint remote_endpoint(boost::system::error_code& error) const override { return _socket.remote_endpoint(error); }
  const char* transport() const noexcept override { return "tcp"; }

 private:
  tcp::socket _socket;
};

}  // namespace

std::unique_ptr<Carrier> socket_carrier(tcp::socket socket) { return std::make_unique<SocketCarrier>(std::move(socket)); }

}  // namespace dannan::listen
