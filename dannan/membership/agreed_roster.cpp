//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/membership/agreed_roster.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "dannan/net/codec.h"

namespace dannan::membership {
namespace {

constexpr std::string_view magic = "DNNAGR01";

}  // namespace

Partition read_partition(const std::set<identity::NodeId>& members, const std::set<identity::NodeId>& active, uint64_t unix_ms) {
  Partition out;
  out.members = members.size();
  out.unix_ms = unix_ms;
  for (const auto& id : members) {
    if (active.contains(id))
      ++out.reachable;
    else
      out.unreachable.push_back(id);
  }
  out.majority = out.reachable * 2 > out.members;
  return out;
}

AgreedRoster::AgreedRoster(identity::NodeId self, std::filesystem::path path) : _self(self), _path(std::move(path)) {
  contract::Lock lock(_mutex);
  _members.insert(_self);
  if (_path.empty() || !std::filesystem::exists(_path)) return;
  std::ifstream file(_path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read the agreed roster " + _path.string());
  const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  try {
    net::Reader reader(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
    const auto head = reader.view(magic.size());
    if (std::string_view(reinterpret_cast<const char*>(head.data()), head.size()) != magic) throw net::DecodeError("not an agreed roster");
    const auto count = reader.u32();
    for (uint32_t i = 0; i < count; ++i) {
      identity::NodeId id;
      id.bytes = reader.fixed<sizeof(id.bytes)>();
      _members.insert(id);
    }
    reader.finish();
  } catch (const net::DecodeError& error) {
    throw std::runtime_error("the agreed roster " + _path.string() + " cannot be read: " + error.what());
  }
  _found = true;
}

bool AgreedRoster::join(const std::vector<identity::NodeId>& ids) {
  contract::Lock lock(_mutex);
  bool added = false;
  for (const auto& id : ids) added = _members.insert(id).second || added;
  if (added) _unwritten = true;
  return added;
}

bool AgreedRoster::retire(const identity::NodeId& id) {
  if (id == _self) throw std::invalid_argument("a node cannot retire itself from the agreed roster");
  Encoded encoded;
  {
    contract::Lock lock(_mutex);
    if (!_members.erase(id)) return false;
    encoded = encode_locked();
  }
  write(encoded);
  return true;
}

std::set<identity::NodeId> AgreedRoster::members() const {
  contract::Lock lock(_mutex);
  return _members;
}

void AgreedRoster::flush() {
  Encoded encoded;
  {
    contract::Lock lock(_mutex);
    if (!_unwritten) return;
    encoded = encode_locked();
  }
  write(encoded);
}

AgreedRoster::Encoded AgreedRoster::encode_locked() {
  net::Writer writer;
  writer.raw(std::span(reinterpret_cast<const uint8_t*>(magic.data()), magic.size()));
  writer.u32(static_cast<uint32_t>(_members.size()));
  for (const auto& id : _members) writer.raw(id.bytes);
  const auto bytes = writer.take();
  _unwritten = false;
  return {++_sequence, std::string(bytes.begin(), bytes.end())};
}

void AgreedRoster::write(const Encoded& encoded) {
  try {
    _file.write(encoded.sequence, encoded.bytes);
  } catch (...) {
    contract::Lock lock(_mutex);
    _unwritten = true;
    throw;
  }
}

}  // namespace dannan::membership
