//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/reach/external_ip.h"

#include <arpa/inet.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <cctype>
#include <stdexcept>

namespace dannan::reach {

namespace asio = boost::asio;

std::string external_ip_from_body(std::string_view body) {
  while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front()))) body.remove_prefix(1);
  while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back()))) body.remove_suffix(1);
  const std::string address(body);
  in_addr parsed{};
  if (address.size() > 15 || ::inet_pton(AF_INET, address.c_str(), &parsed) != 1) throw std::runtime_error("the external IP response was not an IPv4 literal");
  return address;
}

std::string checkip_external_ip(std::chrono::milliseconds timeout) {
  static constexpr const char* host = "checkip.amazonaws.com";
  asio::io_context io;
  asio::ssl::context tls(asio::ssl::context::tls_client);
  tls.set_default_verify_paths();
  tls.set_verify_mode(asio::ssl::verify_peer);
  asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
  if (!SSL_set_tlsext_host_name(stream.native_handle(), host)) throw std::runtime_error("cannot set the TLS server name");
  stream.set_verify_callback(asio::ssl::host_name_verification(host));
  asio::ip::tcp::resolver resolver(io);
  std::string response;
  std::string failure;
  bool done = false;
  const std::string request = std::string("GET / HTTP/1.1\r\nHost: ") + host + "\r\nUser-Agent: dannan-public-connectivity/1\r\nConnection: close\r\n\r\n";
  auto fail = [&](const char* stage, const boost::system::error_code& error) {
    failure = std::string("external IP lookup failed (") + stage + "): " + error.message();
    done = true;
  };
  resolver.async_resolve(asio::ip::tcp::v4(), host, "443", [&](const boost::system::error_code& error, const asio::ip::tcp::resolver::results_type& found) {
    if (error) return fail("resolve", error);
    asio::async_connect(stream.lowest_layer(), found, [&](const boost::system::error_code& error, const asio::ip::tcp::endpoint&) {
      if (error) return fail("connect", error);
      stream.async_handshake(asio::ssl::stream_base::client, [&](const boost::system::error_code& error) {
        if (error) return fail("handshake", error);
        asio::async_write(stream, asio::buffer(request), [&](const boost::system::error_code& error, size_t) {
          if (error) return fail("request", error);
          auto buffer = std::make_shared<std::array<char, 1024>>();
          auto read = std::make_shared<std::function<void()>>();
          *read = [&, buffer, read] {
            stream.async_read_some(asio::buffer(*buffer), [&, buffer, read](const boost::system::error_code& error, size_t size) {
              response.append(buffer->data(), size);
              if (response.size() > 4096) return fail("read", asio::error::message_size);
              if (!error) return (*read)();
              done = true;  // end of stream, cleanly or not: the answer is judged below
            });
          };
          (*read)();
        });
      });
    });
  });
  io.run_for(timeout);
  if (!done) throw std::runtime_error("external IP lookup timed out");
  if (!failure.empty()) throw std::runtime_error(failure);
  const auto head_end = response.find("\r\n\r\n");
  if (!response.starts_with("HTTP/1.1 200") || head_end == std::string::npos)
    throw std::runtime_error("external IP lookup answered " + response.substr(0, response.find("\r\n")));
  return external_ip_from_body(std::string_view(response).substr(head_end + 4));
}

}  // namespace dannan::reach
