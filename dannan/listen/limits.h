//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/ip/address.hpp>
#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dannan::listen {

// A token bucket's shape: `per_second` on average, `burst` at once.
struct Rate {
  double per_second = 0.0;
  double burst = 1.0;
};

// One token bucket on injected time. Not thread-safe: its owner's lock guards it.
class Bucket {
 public:
  explicit Bucket(Rate rate, contract::TimePoint now) : _rate(rate), _tokens(rate.burst), _at(now) {}
  // Takes `cost` if there is that much; whether it did.
  bool take(contract::TimePoint now, double cost = 1.0);
  // Whether `cost` could be taken now, taking nothing.
  bool has(contract::TimePoint now, double cost = 1.0);

 private:
  void refill(contract::TimePoint now);
  Rate _rate;
  double _tokens;
  contract::TimePoint _at;
};

struct LimitsConfig {
  // Connections from one address before any credential; none: no bucket.
  std::optional<Rate> per_address;
  // Peers exempt from the per-address bucket: the trusted reverse proxies, whose
  // clients are told apart by forwarded_for() and limited per request instead.
  std::set<boost::asio::ip::address> trusted_proxies;
  // A caller's budget, for a protocol with one identity per connection (MQTT): the
  // hosted system charges it and ties addresses to callers, and a tied address whose
  // caller has nothing left is refused at accept. None: no per-caller limit.
  std::optional<Rate> per_caller;
};

// The limits a hosted system's listeners share (P3). Every one is optional and off
// by default: a hosted system that answers its own "server busy" sets them only as a
// backstop above its own. Thread-safe; waits on nothing.
class Limits {
 public:
  explicit Limits(LimitsConfig config = {}, const contract::TimeSource* time = &contract::steady_time_source());

  enum class Verdict : uint8_t { admitted, address, caller };
  // At accept: whether a connection from `peer` may be served.
  Verdict admit(const boost::asio::ip::address& peer);
  // The per-caller form: `peer`'s connections speak for `caller` from now; charging
  // `caller` spends its budget, true while there was enough. Without a per-caller
  // rate both do nothing (and charge says true).
  void tie(const boost::asio::ip::address& peer, const std::string& caller);
  void untie(const boost::asio::ip::address& peer);
  bool charge(const std::string& caller, double cost = 1.0);

 private:
  const LimitsConfig _config;
  const contract::TimeSource* const _time;
  contract::Mutex _mutex;
  std::map<boost::asio::ip::address, Bucket> _addresses DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, Bucket, std::less<>> _callers DANNAN_GUARDED_BY(_mutex);
  std::map<boost::asio::ip::address, std::string> _ties DANNAN_GUARDED_BY(_mutex);
};

// The client's address behind trusted proxies: `peer` when it is not one, else the
// nearest address in `x_forwarded_for` (read right to left) that is not one, or the
// leftmost when all are. An entry that is not an address ends the walk at the
// address before it. A helper the hosted system's HTTP code applies; Dannan parses
// no HTTP.
boost::asio::ip::address forwarded_for(std::string_view x_forwarded_for, const std::set<boost::asio::ip::address>& trusted_proxies,
                                       const boost::asio::ip::address& peer);

// The listening socket systemd passed by `name` (LISTEN_PID, LISTEN_FDS,
// LISTEN_FDNAMES), for a privileged port without root; none when there is none. The
// descriptor stays open and is the caller's.
std::optional<int> inherited_socket(std::string_view name);

}  // namespace dannan::listen
