//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/limits.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
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

bool Bucket::full(contract::TimePoint now) {
  refill(now);
  return _tokens >= _rate.burst;
}

Prefix Prefix::parse(std::string_view text) {
  const auto slash = text.find('/');
  boost::system::error_code error;
  Prefix out;
  out.address = boost::asio::ip::make_address(std::string(trimmed(text.substr(0, slash))), error);
  if (error) throw std::invalid_argument("not an address or prefix: " + std::string(text));
  const unsigned bits = out.address.is_v4() ? 32 : 128;
  if (slash == std::string_view::npos) {
    out.length = static_cast<uint8_t>(bits);
    return out;
  }
  const auto length = std::string(trimmed(text.substr(slash + 1)));
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(length.c_str(), &end, 10);
  if (length.empty() || *end != '\0' || parsed > bits) throw std::invalid_argument("not an address or prefix: " + std::string(text));
  out.length = static_cast<uint8_t>(parsed);
  return out;
}

bool Prefix::contains(const boost::asio::ip::address& candidate) const noexcept {
  // An IPv4 peer met on an IPv6 socket is the same address.
  auto peer = candidate;
  if (peer.is_v6() && peer.to_v6().is_v4_mapped()) peer = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, peer.to_v6());
  if (peer.is_v4() != address.is_v4()) return false;
  const auto bytes_of = [](const boost::asio::ip::address& a) {
    std::array<uint8_t, 16> out{};
    if (a.is_v4()) {
      const auto v4 = a.to_v4().to_bytes();
      std::copy(v4.begin(), v4.end(), out.begin());
    } else {
      const auto v6 = a.to_v6().to_bytes();
      std::copy(v6.begin(), v6.end(), out.begin());
    }
    return out;
  };
  const auto ours = bytes_of(address);
  const auto theirs = bytes_of(peer);
  const unsigned whole = length / 8;
  for (unsigned i = 0; i < whole; ++i)
    if (ours[i] != theirs[i]) return false;
  if (const unsigned rest = length % 8; rest) {
    const auto mask = static_cast<uint8_t>(0xffu << (8 - rest));
    if ((ours[whole] & mask) != (theirs[whole] & mask)) return false;
  }
  return true;
}

bool within(const std::vector<Prefix>& prefixes, const boost::asio::ip::address& address) noexcept {
  return std::any_of(prefixes.begin(), prefixes.end(), [&](const Prefix& prefix) { return prefix.contains(address); });
}

Limits::Limits(LimitsConfig config, const contract::TimeSource* time) : _config(std::move(config)), _time(time) {
  if (_config.max_tracked == 0) throw std::invalid_argument("listen limits keep at least one entry (max_tracked)");
}

template <typename Map>
void Limits::bound(Map& map, contract::TimePoint now) {
  if (map.size() <= _config.max_tracked) return;
  // First what forgetting loses nothing of: buckets refilled to their burst.
  if constexpr (std::is_same_v<typename Map::mapped_type, Bucket>) {
    for (auto it = map.begin(); it != map.end();) it = it->second.full(now) ? map.erase(it) : std::next(it);
  }
  // Then the rest, from the front, counted: a debt forgotten.
  while (map.size() > _config.max_tracked) {
    map.erase(map.begin());
    ++_evicted;
  }
}

Limits::Verdict Limits::admit(const boost::asio::ip::address& peer) {
  const auto now = _time->now();
  contract::Lock lock(_mutex);
  if (_config.per_caller) {
    if (const auto tie = _ties.find(peer); tie != _ties.end()) {
      const auto bucket = _callers.find(tie->second);
      if (bucket != _callers.end() && !bucket->second.has(now)) return Verdict::caller;
    }
  }
  if (_config.per_address && !within(_config.trusted_proxies, peer)) {
    auto [bucket, fresh] = _addresses.try_emplace(peer, *_config.per_address, now);
    const bool admitted = bucket->second.take(now);
    if (fresh) bound(_addresses, now);
    if (!admitted) return Verdict::address;
  }
  return Verdict::admitted;
}

void Limits::tie(const boost::asio::ip::address& peer, const std::string& caller) {
  if (!_config.per_caller) return;
  contract::Lock lock(_mutex);
  _ties[peer] = caller;
  bound(_ties, _time->now());
}

size_t Limits::tracked() const {
  contract::Lock lock(_mutex);
  return _addresses.size() + _callers.size() + _ties.size();
}

uint64_t Limits::evicted() const {
  contract::Lock lock(_mutex);
  return _evicted;
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
  const bool taken = bucket->second.take(now, cost);
  if (fresh) bound(_callers, now);
  return taken;
}

boost::asio::ip::address forwarded_for(std::string_view x_forwarded_for, const std::vector<Prefix>& trusted_proxies, const boost::asio::ip::address& peer) {
  // Only a trusted proxy's word is taken: from anyone else the header is forged.
  if (!within(trusted_proxies, peer)) return peer;
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
    if (!within(trusted_proxies, nearest)) return nearest;
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
