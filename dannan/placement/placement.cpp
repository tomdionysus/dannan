//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/placement/placement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

#include "dannan/identity/crypto.h"

namespace dannan::placement {
namespace {

using Bytes = std::vector<uint8_t>;
using u128 = unsigned __int128;

Bytes concat(std::span<const uint8_t> a, std::span<const uint8_t> b) {
  Bytes out(a.begin(), a.end());
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

std::span<const uint8_t> text(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

uint64_t saturating_add(uint64_t a, uint64_t b) { return b > std::numeric_limits<uint64_t>::max() - a ? std::numeric_limits<uint64_t>::max() : a + b; }

Shard shard_of(std::span<const uint8_t> key) {
  std::array<uint8_t, 4> head{};
  if (key.size() >= head.size())
    std::copy_n(key.begin(), head.size(), head.begin());
  else
    std::copy_n(identity::sha256(key).bytes.begin(), head.size(), head.begin());
  return (Shard{head[0]} << 24) | (Shard{head[1]} << 16) | (Shard{head[2]} << 8) | Shard{head[3]};
}

std::array<uint8_t, 4> shard_bytes(std::span<const uint8_t> key) {
  const auto shard = shard_of(key);
  return {static_cast<uint8_t>(shard >> 24), static_cast<uint8_t>(shard >> 16), static_cast<uint8_t>(shard >> 8), static_cast<uint8_t>(shard)};
}

// The water level L that R copies fill: each entity holds min(capacity, L)
// and the total is R * L. Kept as a fraction, exactly.
struct Level {
  u128 numerator = 0;
  uint64_t denominator = 1;
};

Level water_level(const std::vector<uint64_t>& capacities, size_t replicas) {
  if (capacities.empty() || !replicas) return {};
  replicas = std::min(replicas, capacities.size());
  auto sorted = capacities;
  std::sort(sorted.begin(), sorted.end());
  u128 prefix = 0;
  const auto n = static_cast<int64_t>(sorted.size());
  for (size_t k = 0; k < sorted.size(); ++k) {
    prefix += sorted[k];
    // The k+1 smallest are below the level; the rest are capped at it.
    const int64_t denominator = static_cast<int64_t>(replicas) - n + static_cast<int64_t>(k) + 1;
    if (denominator <= 0) continue;
    if (k + 1 == sorted.size() || prefix <= static_cast<u128>(denominator) * sorted[k + 1]) return {prefix, static_cast<uint64_t>(denominator)};
  }
  return {};
}

// How many of the R * 2^32 replica slots each entity owns: in proportion to
// min(capacity, L), largest remainders rounded up, ties to the lower index.
std::vector<uint64_t> quotas(const std::vector<uint64_t>& capacities, size_t replicas) {
  const size_t n = capacities.size();
  std::vector<uint64_t> out(n);
  if (!n || !replicas) return out;
  replicas = std::min(replicas, n);
  const u128 required = static_cast<u128>(replicas) * shard_count;
  const auto level = water_level(capacities, replicas);
  if (!level.numerator) {
    // No capacity to weigh by: spread evenly, in order.
    const auto base = static_cast<uint64_t>(required / n);
    auto extra = static_cast<uint64_t>(required % n);
    for (auto& quota : out) {
      quota = base + (extra ? 1 : 0);
      if (extra) --extra;
    }
    return out;
  }
  std::vector<std::pair<size_t, u128>> remainders;
  u128 assigned = 0;
  for (size_t i = 0; i < n; ++i) {
    const u128 scaled = static_cast<u128>(capacities[i]) * level.denominator;
    if (scaled >= level.numerator) {
      out[i] = shard_count;
      assigned += shard_count;
      remainders.emplace_back(i, 0);
      continue;
    }
    const u128 exact = scaled * shard_count;
    out[i] = static_cast<uint64_t>(exact / level.numerator);
    assigned += out[i];
    remainders.emplace_back(i, exact % level.numerator);
  }
  std::sort(remainders.begin(), remainders.end(), [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
  for (const auto& [index, _] : remainders) {
    if (assigned >= required) break;
    if (out[index] < shard_count) {
      ++out[index];
      ++assigned;
    }
  }
  for (size_t i = 0; assigned < required && i < n; ++i) {
    if (out[i] < shard_count) {
      ++out[i];
      ++assigned;
    }
  }
  return out;
}

// The quotas lay the R * 2^32 slots end to end; the key's shard s picks the
// owners of s, s + 2^32, ...: one per copy, each entity chosen exactly as
// often as its quota over the whole space.
std::vector<size_t> sample(std::span<const uint8_t> key, const std::vector<uint64_t>& capacities, size_t replicas) {
  std::vector<size_t> picked;
  if (capacities.empty() || !replicas) return picked;
  replicas = std::min(replicas, capacities.size());
  const auto owned = quotas(capacities, replicas);
  const uint64_t shard = shard_of(key);
  u128 begin = 0;
  for (size_t i = 0; i < owned.size(); ++i) {
    const u128 end = begin + owned[i];
    for (size_t copy = 0; copy < replicas; ++copy) {
      const u128 point = shard + static_cast<u128>(copy) * shard_count;
      if (point >= begin && point < end) {
        picked.push_back(i);
        break;
      }
    }
    begin = end;
  }
  for (size_t i = 0; i < capacities.size() && picked.size() < replicas; ++i)
    if (std::find(picked.begin(), picked.end(), i) == picked.end()) picked.push_back(i);
  return picked;
}

struct Domain {
  std::string name;
  uint64_t capacity = 0;
  std::vector<NodeRecord> nodes;
};

// Failure domains in name order, each node alone in one if it names none;
// nodes within a domain in id order.
std::vector<Domain> domains_of(const std::vector<NodeRecord>& nodes, const std::string& advert) {
  std::vector<Domain> out;
  for (const auto& node : nodes) {
    const auto capacity = capacity_of(node, advert);
    const auto name = node.failure_domain.empty() ? "node:" + identity::to_string(node.id) : "domain:" + node.failure_domain;
    const auto found = std::find_if(out.begin(), out.end(), [&](const Domain& d) { return d.name == name; });
    if (found == out.end()) {
      out.push_back({name, capacity, {node}});
    } else {
      found->capacity = saturating_add(found->capacity, capacity);
      found->nodes.push_back(node);
    }
  }
  std::sort(out.begin(), out.end(), [](const Domain& a, const Domain& b) { return a.name < b.name; });
  for (auto& domain : out) std::sort(domain.nodes.begin(), domain.nodes.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  return out;
}

std::vector<uint64_t> capacities(const std::vector<Domain>& domains) {
  std::vector<uint64_t> out;
  for (const auto& domain : domains) out.push_back(domain.capacity);
  return out;
}

std::vector<uint64_t> capacities(const std::vector<NodeRecord>& nodes, const std::string& advert) {
  std::vector<uint64_t> out;
  for (const auto& node : nodes) out.push_back(capacity_of(node, advert));
  return out;
}

NodeRecord within(std::span<const uint8_t> key, const Domain& domain, const std::string& advert) {
  if (domain.nodes.size() == 1) return domain.nodes.front();
  const auto inner = identity::sha256(concat(key, text(domain.name)));
  const auto picked = sample(inner.bytes, capacities(domain.nodes, advert), 1);
  return domain.nodes[picked.empty() ? 0 : picked.front()];
}

// Weighted rendezvous: -ln(u) / capacity with u uniform in (0, 1) from the
// hash; the lowest wins. A node with no capacity weighs 1.
long double fallback_score(std::span<const uint8_t> key, const NodeRecord& node, std::string_view salt, const std::string& advert) {
  const auto hash = identity::sha256(concat(concat(key, text(salt)), node.id.bytes));
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value = (value << 8) | hash.bytes[i];
  const long double u = (static_cast<long double>(value) + 1.0L) / (static_cast<long double>(std::numeric_limits<uint64_t>::max()) + 2.0L);
  const auto capacity = capacity_of(node, advert);
  const long double weight = capacity ? static_cast<long double>(capacity) : 1.0L;
  return -std::log(u) / weight;
}

void sort_by_fallback(std::vector<NodeRecord>& nodes, std::span<const uint8_t> key, std::string_view salt, const std::string& advert) {
  std::sort(nodes.begin(), nodes.end(), [&](const NodeRecord& a, const NodeRecord& b) {
    const auto as = fallback_score(key, a, salt, advert);
    const auto bs = fallback_score(key, b, salt, advert);
    return as != bs ? as < bs : a.id < b.id;
  });
}

}  // namespace

uint64_t capacity_of(const NodeRecord& node, const std::string& advert) { return advert.empty() ? 0 : node.advert_u64(advert).value_or(0); }

std::vector<NodeRecord> rendezvous(std::span<const uint8_t> key, const std::vector<NodeRecord>& nodes, size_t count) {
  std::vector<std::pair<identity::Hash256, const NodeRecord*>> scored;
  for (const auto& node : nodes) scored.emplace_back(identity::sha256(concat(key, node.id.bytes)), &node);
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second->id > b.second->id; });
  count = std::min(count, scored.size());
  std::vector<NodeRecord> out;
  std::set<std::string> domains;
  std::set<identity::NodeId> chosen;
  for (const auto& [_, node] : scored) {
    if (out.size() == count) break;
    const auto domain = node->failure_domain.empty() ? identity::to_string(node->id) : node->failure_domain;
    if (domains.insert(domain).second) {
      out.push_back(*node);
      chosen.insert(node->id);
    }
  }
  for (const auto& [_, node] : scored) {
    if (out.size() == count) break;
    if (chosen.insert(node->id).second) out.push_back(*node);
  }
  return out;
}

std::vector<NodeRecord> by_capacity(std::span<const uint8_t> key, const std::vector<NodeRecord>& nodes, size_t count, const Scheme& scheme) {
  if (scheme.fallback_salt.empty() || scheme.extra_domain_salt.empty()) throw std::invalid_argument("a placement scheme names both its salts");
  if (nodes.empty() || !count) return {};
  const auto& advert = scheme.capacity_advert;
  count = std::min(count, nodes.size());
  if (count == 1) {
    auto out = nodes;
    sort_by_fallback(out, shard_bytes(key), scheme.fallback_salt, advert);
    return out;
  }
  const auto domains = domains_of(nodes, advert);
  std::vector<NodeRecord> out;
  std::set<identity::NodeId> chosen;
  for (const auto index : sample(key, capacities(domains), std::min(count, domains.size()))) {
    auto node = within(key, domains[index], advert);
    if (chosen.insert(node.id).second) out.push_back(std::move(node));
  }
  // Fewer domains than copies: one node from every domain is in; the rest
  // come from the unused nodes, by capacity.
  if (out.size() < count) {
    std::vector<NodeRecord> remaining;
    for (const auto& node : nodes)
      if (!chosen.contains(node.id)) remaining.push_back(node);
    std::sort(remaining.begin(), remaining.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    const auto extra_key = identity::sha256(concat(key, text(scheme.extra_domain_salt)));
    for (const auto index : sample(extra_key.bytes, capacities(remaining, advert), count - out.size()))
      if (chosen.insert(remaining[index].id).second) out.push_back(remaining[index]);
  }
  std::vector<NodeRecord> fallback;
  for (const auto& node : nodes)
    if (!chosen.contains(node.id)) fallback.push_back(node);
  sort_by_fallback(fallback, key, scheme.fallback_salt, advert);
  out.insert(out.end(), fallback.begin(), fallback.end());
  return out;
}

uint64_t logical_capacity(const std::vector<NodeRecord>& nodes, size_t replicas, const std::string& capacity_advert) {
  if (nodes.empty() || !replicas) return 0;
  replicas = std::min(replicas, nodes.size());
  const auto domains = domains_of(nodes, capacity_advert);
  const auto level = water_level(domains.size() >= replicas ? capacities(domains) : capacities(nodes, capacity_advert), replicas);
  if (!level.numerator) return 0;
  const u128 value = level.numerator / level.denominator;
  return value > std::numeric_limits<uint64_t>::max() ? std::numeric_limits<uint64_t>::max() : static_cast<uint64_t>(value);
}

std::vector<NodeRecord> order_commit_replicas(std::vector<NodeRecord> replicas, const identity::NodeId& local,
                                              const std::function<std::optional<std::chrono::milliseconds>(const identity::NodeId&)>& latency) {
  const auto rank = [&](const NodeRecord& node) -> std::pair<int, int64_t> {
    if (node.id == local) return {0, 0};
    if (const auto measured = latency ? latency(node.id) : std::nullopt) return {1, measured->count()};
    return {2, 0};
  };
  std::stable_sort(replicas.begin(), replicas.end(), [&](const NodeRecord& a, const NodeRecord& b) { return rank(a) < rank(b); });
  return replicas;
}

std::vector<NodeRecord> with_advert(const std::vector<NodeRecord>& nodes, const std::string& key) {
  std::vector<NodeRecord> out;
  for (const auto& node : nodes)
    if (node.adverts.contains(key)) out.push_back(node);
  return out;
}

}  // namespace dannan::placement
