//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/limits.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace dannan::listen {

namespace {

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// One X-Forwarded-For entry as an address: "[v6]" and "v4:port" are taken too.
std::optional<boost::asio::ip::address> address_of(std::string_view entry) {
  entry = trimmed(entry);
  if (entry.size() > 2 && entry.front() == '[') {
    const auto close = entry.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    entry = entry.substr(1, close - 1);
  } else if (std::count(entry.begin(), entry.end(), ':') == 1) {
    entry = entry.substr(0, entry.find(':'));
  }
  boost::system::error_code error;
  const auto address = boost::asio::ip::make_address(std::string(entry), error);
  if (error) return std::nullopt;
  return address;
}

}  // namespace

void Bucket::refill(contract::TimePoint now) {
  if (now <= _at) return;
  const double seconds = std::chrono::duration<double>(now - _at).count();
  _tokens = std::min(_rate.burst, _tokens + seconds * _rate.per_second);
  _at = now;
}

bool Bucket::take(contract::TimePoint now, double cost) {
  refill(now);
  if (_tokens < cost) return false;
  _tokens -= cost;
  return true;
}

bool Bucket::has(contract::TimePoint now, double cost) {
  refill(now);
  return _tokens >= cost;
}

Limits::Limits(LimitsConfig config, const contract::TimeSource* time) : _config(std::move(config)), _time(time) {}

Limits::Verdict Limits::admit(const boost::asio::ip::address& peer) {
  const auto now = _time->now();
  contract::Lock lock(_mutex);
  if (_config.per_caller) {
    if (const auto tie = _ties.find(peer); tie != _ties.end()) {
      const auto bucket = _callers.find(tie->second);
      if (bucket != _callers.end() && !bucket->second.has(now)) return Verdict::caller;
    }
  }
  if (_config.per_address && !_config.trusted_proxies.contains(peer)) {
    auto [bucket, fresh] = _addresses.try_emplace(peer, *_config.per_address, now);
    (void)fresh;
    if (!bucket->second.take(now)) return Verdict::address;
  }
  return Verdict::admitted;
}

void Limits::tie(const boost::asio::ip::address& peer, const std::string& caller) {
  if (!_config.per_caller) return;
  contract::Lock lock(_mutex);
  _ties[peer] = caller;
}

void Limits::untie(const boost::asio::ip::address& peer) {
  contract::Lock lock(_mutex);
  _ties.erase(peer);
}

bool Limits::charge(const std::string& caller, double cost) {
  if (!_config.per_caller) return true;
  const auto now = _time->now();
  contract::Lock lock(_mutex);
  auto [bucket, fresh] = _callers.try_emplace(caller, *_config.per_caller, now);
  (void)fresh;
  return bucket->second.take(now, cost);
}

boost::asio::ip::address forwarded_for(std::string_view x_forwarded_for, const std::set<boost::asio::ip::address>& trusted_proxies,
                                       const boost::asio::ip::address& peer) {
  // Only a trusted proxy's word is taken: from anyone else the header is forged.
  if (!trusted_proxies.contains(peer)) return peer;
  std::vector<std::string_view> entries;
  for (std::string_view rest = x_forwarded_for; !rest.empty();) {
    const auto comma = rest.find(',');
    entries.push_back(rest.substr(0, comma));
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  }
  auto nearest = peer;
  for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
    if (trimmed(*entry).empty()) continue;
    const auto address = address_of(*entry);
    if (!address) return nearest;
    nearest = *address;
    if (!trusted_proxies.contains(nearest)) return nearest;
  }
  return nearest;
}

std::optional<int> inherited_socket(std::string_view name) {
  const char* pid = std::getenv("LISTEN_PID");
  const char* count = std::getenv("LISTEN_FDS");
  const char* names = std::getenv("LISTEN_FDNAMES");
  if (!pid || !count || !names) return std::nullopt;
  if (std::strtol(pid, nullptr, 10) != static_cast<long>(::getpid())) return std::nullopt;
  const long fds = std::strtol(count, nullptr, 10);
  std::string_view rest = names;
  for (long index = 0; index < fds && !rest.empty(); ++index) {
    const auto colon = rest.find(':');
    if (rest.substr(0, colon) == name) return static_cast<int>(3 + index);
    if (colon == std::string_view::npos) break;
    rest = rest.substr(colon + 1);
  }
  return std::nullopt;
}

}  // namespace dannan::listen
