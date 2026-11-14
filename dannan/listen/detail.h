//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// The listener's own: what it and its streams share. Not for hosted systems.

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/listen/certificates.h"
#include "dannan/listen/limits.h"
#include "dannan/listen/listener.h"

namespace dannan::listen {

// One load of the operator's files, as read: the key cleared from memory when the
// last listener lets the load go.
struct Certificates::Material {
  Material() = default;
  ~Material();
  Material(const Material&) = delete;
  Material& operator=(const Material&) = delete;
  std::string chain;
  std::string key;
  std::string client_ca;
};

namespace detail {

// A server context built from one load under one policy, with the ALPN list its
// selection reads.
struct ServerTls {
  explicit ServerTls(boost::asio::ssl::context::method method) : context(method) {}
  boost::asio::ssl::context context;
  std::string alpn_wire;
  bool client_bundle = false;
};

// Throws std::runtime_error naming the listener, never a value.
std::shared_ptr<ServerTls> server_tls(const Certificates::Material& material, const TlsPolicy& policy, const std::string& listener);

// Runs `fire` on `timer`'s executor once `time` reaches `at`, unless the timer is
// cancelled first. Looks again as the source says (a stepped source moves without
// waking a timer).
void when_time_reaches(const std::shared_ptr<boost::asio::steady_timer>& timer, const contract::TimeSource& time, contract::TimePoint at,
                       std::function<void()> fire);

}  // namespace detail

struct ListenerShared {
  ListenerShared(boost::asio::io_context& io, ListenerConfig given, const contract::TimeSource* source)
      : config(std::move(given)), time(source), acceptor(boost::asio::make_strand(io)) {}

  // The TLS context for the material as it is now, rebuilt after a reload.
  std::shared_ptr<detail::ServerTls> current_tls();

  const ListenerConfig config;
  const contract::TimeSource* const time;
  // On its strand: accept, stop_accepting.
  boost::asio::ip::tcp::acceptor acceptor;
  boost::asio::any_io_executor executor;
  std::function<void(std::shared_ptr<Stream>)> accepted;

  mutable contract::Mutex mutex;
  ListenerCounters counters DANNAN_GUARDED_BY(mutex);
  std::vector<std::weak_ptr<Stream>> open DANNAN_GUARDED_BY(mutex);
  std::optional<Bucket> accept_bucket DANNAN_GUARDED_BY(mutex);
  std::shared_ptr<detail::ServerTls> tls DANNAN_GUARDED_BY(mutex);
  uint64_t tls_generation DANNAN_GUARDED_BY(mutex) = 0;
};

}  // namespace dannan::listen
