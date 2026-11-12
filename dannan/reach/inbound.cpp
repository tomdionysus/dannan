//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/reach/inbound.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

#include "dannan/identity/durable_file.h"

namespace dannan::reach {
namespace {

constexpr std::array<uint8_t, 8> magic{'D', 'N', 'N', 'I', 'N', 'B', '0', '1'};

}  // namespace

InboundResolver::InboundResolver(Inbound mode, std::filesystem::path state, const contract::WallClock* wall, std::chrono::milliseconds reprobe_while_capable,
                                 std::chrono::milliseconds reprobe_while_incapable, std::chrono::milliseconds confirm)
    : _path(state / "connectivity" / "inbound.bin"),
      _wall(wall),
      _reprobe_capable(reprobe_while_capable),
      _reprobe_incapable(reprobe_while_incapable),
      _confirm(confirm) {
  contract::Lock lock(_mutex);
  _state.mode = mode;
  if (mode != Inbound::automatic) {
    _state.capable = mode == Inbound::yes;
    _state.source = "configured";
    return;
  }
  _state.source = "default";
  try {
    if (!std::filesystem::exists(_path)) return;
    std::ifstream input(_path, std::ios::binary);
    const net::Bytes bytes((std::istreambuf_iterator<char>(input)), {});
    net::Reader reader(bytes);
    if (reader.fixed<8>() != magic) throw net::DecodeError("bad inbound resolution magic");
    const bool capable = reader.u8() != 0;
    const auto decided = reader.u64();
    (void)reader.string(4096);  // who decided it: informational
    reader.finish();
    _state.capable = capable;
    _state.decided_unix_ms = decided;
    _state.source = "persisted";
  } catch (const std::exception& error) {
    // Persisted evidence is a convenience; the next probe decides again.
    _load_error = error.what();
    _state.capable = true;
    _state.source = "default";
  }
}

bool InboundResolver::capable() const {
  contract::Lock lock(_mutex);
  return _state.capable;
}

InboundState InboundResolver::state() const {
  contract::Lock lock(_mutex);
  return _state;
}

ProbeVerdict InboundResolver::observe_probe(const identity::NodeId& peer, bool reachable, const std::string& error) {
  ProbeVerdict verdict;
  InboundState after;
  {
    contract::Lock lock(_mutex);
    if (_state.mode != Inbound::automatic) return verdict;
    _state.last_probe_unix_ms = _wall->unix_ms();
    _state.last_probe_peer = to_string(peer);
    _state.last_probe_error = reachable ? std::string() : error;
    _state.consecutive_failures = reachable ? 0 : _state.consecutive_failures + 1;
    bool capable = _state.capable;
    if (reachable) {
      capable = true;
      verdict.next_probe = _reprobe_capable;
    } else if (_state.capable && _state.consecutive_failures < 2) {
      // One failure is not a verdict: confirm on the next round.
      verdict.next_probe = _confirm;
      return verdict;
    } else {
      capable = false;
      verdict.next_probe = _reprobe_incapable;
    }
    const auto source = "probe:" + to_string(peer);
    verdict.changed = _state.capable != capable || _state.source != source;
    _state.capable = capable;
    _state.source = source;
    if (verdict.changed) _state.decided_unix_ms = _wall->unix_ms();
    after = _state;
  }
  try {
    persist(after);
  } catch (const std::exception& failed) {
    verdict.persist_error = failed.what();
  }
  return verdict;
}

void InboundResolver::persist(const InboundState& state) {
  contract::Lock lock(_persist_mutex);
  std::filesystem::create_directories(_path.parent_path());
  net::Writer writer;
  writer.raw(magic);
  writer.u8(state.capable ? 1 : 0);
  writer.u64(state.decided_unix_ms);
  writer.string(state.source);
  identity::durable_replace_file(_path, std::string_view(reinterpret_cast<const char*>(writer.data().data()), writer.data().size()));
}

void refuse_impossible_cluster(Inbound mode, const std::vector<net::Endpoint>& bootstrap, const std::vector<membership::NodeRecord>& known,
                               const identity::NodeId& self) {
  if (mode != Inbound::no) return;
  if (bootstrap.empty())
    throw std::runtime_error(
        "network.inbound_capable is false and no bootstrap peers are configured: a founding node must accept inbound connections, or nothing could ever "
        "join this cluster");
  // Only a bootstrap peer met before can be known to be incapable.
  for (const auto& endpoint : bootstrap) {
    const auto found = std::find_if(known.begin(), known.end(), [&](const membership::NodeRecord& node) {
      return node.id != self && node.host == endpoint.host && node.port == endpoint.port;
    });
    if (found == known.end() || found->inbound_capable()) return;
  }
  throw std::runtime_error(
      "network.inbound_capable is false and every bootstrap peer is known to accept no inbound connections either: no node in this cluster could be reached "
      "by anyone");
}

}  // namespace dannan::reach
