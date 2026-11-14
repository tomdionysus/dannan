//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// Beast over a listen::Stream (Lir's review): what Beast's WebSocket finds by
// argument-dependent lookup when it ends a connection, or times one out. Beast
// defines these for its own sockets and TLS streams only. Include it where a
// websocket::stream<listen::Stream&> is made; the rest of Dannan does not need Beast.

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/role.hpp>
#include <utility>

#include "dannan/listen/stream.h"

namespace dannan::listen {

// The WebSocket's end of the connection, once its closing handshake is done: the
// stream's clean close. Not waited for: a synchronous WebSocket does not run on a
// listener's streams, whose work is asynchronous.
inline void teardown(boost::beast::role_type, Stream& stream, boost::beast::error_code& error) {
  stream.close();
  error = {};
}

// The same, asynchronously: the stream's clean close (what send() queued, then
// close_notify, within the close bound), the handler told once it has finished, on
// its own executor.
template <class TeardownHandler>
void async_teardown(boost::beast::role_type, Stream& stream, TeardownHandler&& handler) {
  auto executor = boost::asio::get_associated_executor(handler, stream.get_executor());
  stream.when_finished([executor, handler = std::forward<TeardownHandler>(handler)]() mutable {
    boost::asio::post(executor, [handler = std::move(handler)]() mutable { std::move(handler)(boost::beast::error_code{}); });
  });
  stream.close();
}

// A WebSocket's timeout ends its lowest layer at once.
inline void beast_close_socket(Stream& stream) { stream.abort(); }

}  // namespace dannan::listen
