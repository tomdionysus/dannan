//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/membership/membership.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

#include "dannan/identity/durable_file.h"

namespace dannan::membership {
namespace {

using Magic = std::array<uint8_t, 8>;
constexpr Magic magic{'D', 'N', 'N', 'M', 'E', 'M', '0', '1'};
// How far a last-heard time may move before the roster is rewritten for it.
constexpr uint64_t heard_persist_step_ms = 60ull * 60 * 1000;
constexpr uint32_t max_nodes = 65536;
constexpr uint32_t max_resets = 65536;
constexpr uint64_t max_bytes = 16ull * 1024 * 1024;

}  // namespace

Membership::Membership(NodeRecord self, std::chrono::milliseconds dead_after, std::filesystem::path known_path, std::chrono::milliseconds forget_after,
                       const contract::TimeSource* time, const contract::WallClock* wall)
    : _dead_after(dead_after), _forget_after(std::max(forget_after, 2 * dead_after)), _path(std::move(known_path)), _time(time), _wall(wall) {
  contract::Lock lock(_mutex);
  _self = std::move(self);
  load();
}

void Membership::load() {
  if (_path.empty() || !std::filesystem::exists(_path)) return;
  const auto size = std::filesystem::file_size(_path);
  if (size > max_bytes) throw std::runtime_error("known-node roster is too large");
  std::ifstream input(_path, std::ios::binary);
  net::Bytes bytes(static_cast<size_t>(size));
  if (!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))))
    throw std::runtime_error("cannot read known-node roster " + _path.string());
  net::Reader reader(bytes);
  if (reader.fixed<8>() != magic) throw net::DecodeError("bad known-node roster magic");
  const auto count = reader.u32();
  if (count > max_nodes) throw net::DecodeError("too many known nodes");
  // Known, not live: a reloaded node is active only once heard from again.
  // When it was last heard of comes back from the wall time persisted, so
  // its absence keeps its length across the restart.
  const auto now = _time->now();
  const auto wall = _wall->unix_ms();
  const auto stale = now - _dead_after - std::chrono::milliseconds(1);
  for (uint32_t i = 0; i < count; ++i) {
    auto node = decode(reader);
    const uint64_t heard = reader.u64();
    if (node.id == identity::NodeId{} || node.id == _self.id || node.host.empty() || !node.port) throw net::DecodeError("bad known-node roster entry");
    const auto id = node.id;
    const auto seen = wall > heard ? std::min(stale, now - std::chrono::milliseconds(wall - heard)) : stale;
    if (!_nodes.emplace(id, Known{std::move(node), seen, std::nullopt, heard, heard}).second) throw net::DecodeError("duplicate known-node roster entry");
  }
  const auto resets = reader.u32();
  if (resets > max_resets) throw net::DecodeError("too many identity association resets");
  for (uint32_t i = 0; i < resets; ++i) {
    IdentityReset reset;
    reset.host = reader.string(4096);
    reset.port = reader.u16();
    reset.stale_node_id.bytes = reader.fixed<16>();
    reset.epoch = reader.u64();
    reset.reset_unix_ms = reader.u64();
    reset.reset_by.bytes = reader.fixed<16>();
    reset.reason = reader.string(4096);
    if (reset.host.empty() || !reset.epoch) throw net::DecodeError("bad identity association reset");
    const auto key = reset.key();
    if (!_resets.emplace(key, std::move(reset)).second) throw net::DecodeError("duplicate identity association reset");
  }
  std::erase_if(_nodes, [&](const auto& item) DANNAN_REQUIRES(_mutex) {
    const auto& node = item.second.record;
    return std::any_of(_resets.begin(), _resets.end(), [&](const auto& held) {
      const auto& reset = held.second;
      return reset.matches_endpoint(node.host, node.port) && reset.matches_node(node.id) && node.seen_unix_ms <= reset.reset_unix_ms;
    });
  });
  reader.finish();
}

Membership::Encoded Membership::encode_locked() {
  if (_path.empty()) return {};
  if (_nodes.size() > max_nodes) throw std::runtime_error("too many known nodes");
  if (_resets.size() > max_resets) throw std::runtime_error("too many identity association resets");
  std::vector<const Known*> ordered;
  ordered.reserve(_nodes.size());
  for (const auto& [_, known] : _nodes) ordered.push_back(&known);
  std::sort(ordered.begin(), ordered.end(), [](const Known* a, const Known* b) { return a->record.id < b->record.id; });
  net::Writer writer;
  writer.raw(magic);
  writer.u32(static_cast<uint32_t>(ordered.size()));
  for (const auto* known : ordered) {
    encode(writer, known->record);
    writer.u64(known->last_heard_unix_ms);
  }
  std::vector<const IdentityReset*> resets;
  for (const auto& [_, reset] : _resets) resets.push_back(&reset);
  std::sort(resets.begin(), resets.end(), [](const IdentityReset* a, const IdentityReset* b) { return a->key() < b->key(); });
  writer.u32(static_cast<uint32_t>(resets.size()));
  for (const auto* reset : resets) {
    writer.string(reset->host);
    writer.u16(reset->port);
    writer.raw(reset->stale_node_id.bytes);
    writer.u64(reset->epoch);
    writer.u64(reset->reset_unix_ms);
    writer.raw(reset->reset_by.bytes);
    writer.string(reset->reason);
  }
  const auto& bytes = writer.data();
  if (bytes.size() > max_bytes) throw std::runtime_error("known-node roster is too large");
  for (auto& [_, known] : _nodes) known.persisted_heard_unix_ms = known.last_heard_unix_ms;
  return {++_sequence, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
}

void Membership::write(const Encoded& roster) {
  if (_path.empty()) return;
  contract::Lock lock(_persist_mutex);
  // Encoded later, written already: this one is older.
  if (roster.sequence <= _written) return;
  identity::durable_replace_file(_path, roster.bytes);
  _written = roster.sequence;
}

NodeRecord Membership::self() const {
  contract::Lock lock(_mutex);
  auto out = _self;
  out.seen_unix_ms = _wall->unix_ms();
  return out;
}

void Membership::set_endpoint(std::string host, uint16_t port) {
  if (host.empty() || !port) throw std::invalid_argument("a membership endpoint needs a host and a port");
  contract::Lock lock(_mutex);
  _self.host = std::move(host);
  _self.port = port;
  _self.seen_unix_ms = _wall->unix_ms();
}

bool Membership::set_capabilities(uint32_t capabilities) {
  contract::Lock lock(_mutex);
  if (_self.capabilities == capabilities) return false;
  _self.capabilities = capabilities;
  _self.seen_unix_ms = _wall->unix_ms();
  return true;
}

bool Membership::set_inbound_capable(bool capable) {
  contract::Lock lock(_mutex);
  const auto capabilities = capable ? _self.capabilities | capability_inbound : _self.capabilities & ~capability_inbound;
  if (_self.capabilities == capabilities) return false;
  _self.capabilities = capabilities;
  _self.seen_unix_ms = _wall->unix_ms();
  return true;
}

bool Membership::set_path_class(PathClass path) {
  contract::Lock lock(_mutex);
  if (_self.path_class == path) return false;
  _self.path_class = path;
  _self.seen_unix_ms = _wall->unix_ms();
  return true;
}

bool Membership::set_candidates(std::vector<Candidate> candidates) {
  if (candidates.size() > max_candidates) throw std::invalid_argument("too many candidates");
  contract::Lock lock(_mutex);
  if (_self.candidates == candidates) return false;
  _self.candidates = std::move(candidates);
  _self.seen_unix_ms = _wall->unix_ms();
  return true;
}

bool Membership::set_advert(const std::string& key, net::Bytes value) {
  if (key.empty() || key.size() > max_advert_key) throw std::invalid_argument("an advert key is 1 to 256 bytes");
  if (value.size() > max_advert_value) throw std::invalid_argument("an advert is at most 1 KiB");
  contract::Lock lock(_mutex);
  const auto found = _self.adverts.find(key);
  if (found != _self.adverts.end() && found->second == value) return false;
  if (found == _self.adverts.end() && _self.adverts.size() >= max_adverts) throw std::invalid_argument("a node advertises at most 64 things");
  _self.adverts[key] = std::move(value);
  _self.seen_unix_ms = _wall->unix_ms();
  return true;
}

bool Membership::clear_advert(const std::string& key) {
  contract::Lock lock(_mutex);
  if (!_self.adverts.erase(key)) return false;
  _self.seen_unix_ms = _wall->unix_ms();
  return true;
}

bool Membership::inbound_capable(const identity::NodeId& id) const {
  contract::Lock lock(_mutex);
  if (id == _self.id) return _self.inbound_capable();
  const auto found = _nodes.find(id);
  return found == _nodes.end() || found->second.record.inbound_capable();
}

void Membership::observe(NodeRecord record, bool direct) {
  if (record.host.empty() || !record.port) return;
  Encoded roster;
  {
    contract::Lock lock(_mutex);
    if (record.id == _self.id) return;
    for (const auto& [_, reset] : _resets) {
      if (!reset.matches_endpoint(record.host, record.port) || !reset.matches_node(record.id)) continue;
      // Only the node itself, or gossip newer than the reset, brings it back.
      if (!direct && record.seen_unix_ms <= reset.reset_unix_ms) return;
    }
    const auto now = _time->now();
    const auto wall = _wall->unix_ms();
    bool changed = false;
    const auto found = _nodes.find(record.id);
    if (found == _nodes.end()) {
      // Gossip about a node unheard of for the horizon only teaches back
      // what was forgotten.
      if (!direct && wall >= record.seen_unix_ms && wall - record.seen_unix_ms >= static_cast<uint64_t>(_forget_after.count())) return;
      const auto id = record.id;
      _nodes.emplace(id, Known{std::move(record), now, direct ? std::optional<contract::TimePoint>(now) : std::nullopt, wall, 0});
      changed = true;
    } else {
      auto& known = found->second;
      const bool association_changed = known.record.host != record.host || known.record.port != record.port ||
                                       known.record.failure_domain != record.failure_domain || known.record.capabilities != record.capabilities ||
                                       known.record.path_class != record.path_class;
      // A handshake carries the record as it was then, which gossip may have
      // superseded: the newer record wins, direct only refreshes liveness.
      const bool newer = record.seen_unix_ms > known.record.seen_unix_ms;
      if (newer) {
        known.record = std::move(record);
        known.seen = now;
        changed = association_changed;
      } else if (direct) {
        known.seen = now;
      }
      if (direct) known.direct_seen = now;
      if (newer || direct) {
        known.last_heard_unix_ms = wall;
        changed = changed || wall - known.persisted_heard_unix_ms >= heard_persist_step_ms;
      }
    }
    if (!changed) return;
    roster = encode_locked();
  }
  write(roster);
}

bool Membership::apply_identity_reset(const IdentityReset& reset) {
  if (reset.host.empty() || !reset.epoch) return false;
  Encoded roster;
  {
    contract::Lock lock(_mutex);
    const auto key = reset.key();
    if (const auto held = _resets.find(key); held != _resets.end() && held->second.epoch >= reset.epoch) return false;
    _resets[key] = reset;
    std::erase_if(_nodes,
                  [&](const auto& item) { return reset.matches_endpoint(item.second.record.host, item.second.record.port) && reset.matches_node(item.first); });
    // The tombstone is persisted even with no member to drop, so the reset
    // survives a restart.
    roster = encode_locked();
  }
  write(roster);
  return true;
}

std::vector<IdentityReset> Membership::identity_resets() const {
  contract::Lock lock(_mutex);
  std::vector<IdentityReset> out;
  for (const auto& [_, reset] : _resets) out.push_back(reset);
  return out;
}

std::vector<NodeRecord> Membership::all() const {
  contract::Lock lock(_mutex);
  std::vector<NodeRecord> out{_self};
  out[0].seen_unix_ms = _wall->unix_ms();
  for (const auto& [_, known] : _nodes) out.push_back(known.record);
  return out;
}

std::vector<NodeRecord> Membership::active() const {
  contract::Lock lock(_mutex);
  const auto now = _time->now();
  std::vector<NodeRecord> out{_self};
  out[0].seen_unix_ms = _wall->unix_ms();
  for (const auto& [_, known] : _nodes)
    if (now - known.seen <= _dead_after) out.push_back(known.record);
  return out;
}

std::optional<contract::TimePoint> Membership::absent_since(const identity::NodeId& id) const {
  contract::Lock lock(_mutex);
  const auto found = _nodes.find(id);
  if (found == _nodes.end() || _time->now() - found->second.seen <= _dead_after) return std::nullopt;
  return found->second.seen;
}

bool Membership::directly_reachable(const identity::NodeId& id) const {
  contract::Lock lock(_mutex);
  if (id == _self.id) return true;
  const auto found = _nodes.find(id);
  return found != _nodes.end() && found->second.direct_seen && _time->now() - *found->second.direct_seen <= _dead_after;
}

size_t Membership::forget_unseen() {
  Encoded roster;
  size_t forgotten = 0;
  {
    contract::Lock lock(_mutex);
    const auto now = _time->now();
    const auto wall = _wall->unix_ms();
    forgotten = std::erase_if(_nodes, [&](const auto& item) {
      const auto& known = item.second;
      return now - known.seen > _dead_after && wall >= known.last_heard_unix_ms &&
             wall - known.last_heard_unix_ms >= static_cast<uint64_t>(_forget_after.count());
    });
    if (!forgotten) return 0;
    roster = encode_locked();
  }
  write(roster);
  return forgotten;
}

bool Membership::all_known_reachable() const {
  contract::Lock lock(_mutex);
  const auto now = _time->now();
  const bool self_capable = _self.inbound_capable();
  return std::all_of(_nodes.begin(), _nodes.end(), [&](const auto& item) {
    if (!self_capable && !item.second.record.inbound_capable()) return true;
    return item.second.direct_seen && now - *item.second.direct_seen <= _dead_after;
  });
}

}  // namespace dannan::membership
