//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/membership/identity_reset.h"
#include "dannan/membership/node_record.h"

namespace dannan::membership {

// The roster: every node this one knows, from its own connections (direct)
// and from gossip. A node is active while heard of within dead_after, and
// directly reachable while authenticated directly within it; gossip never
// makes a node directly reachable. A node unheard of for the forgetting
// horizon is dropped, and gossip that old does not teach it back. The
// roster is persisted under the state path (DNNMEM01: each record as the
// wire carries it, then when this node last heard of it), so a restarting
// node knows its peers
// and which of them it must not dial before it has heard from anyone.
//
// Thread-safe. Waits only to write the roster file, never while holding
// the roster: it is encoded under _mutex and written under _persist_mutex,
// in sequence order (lock order: _mutex is never taken with
// _persist_mutex held).
class Membership {
 public:
  // `known_path` empty: nothing persisted. `forget_after` is raised to
  // twice dead_after, never sooner than a node is given up for dead. Throws
  // if a roster file exists and cannot be read (DecodeError if corrupt).
  Membership(NodeRecord self, std::chrono::milliseconds dead_after, std::filesystem::path known_path = {},
             std::chrono::milliseconds forget_after = std::chrono::milliseconds::max(), const contract::TimeSource* time = &contract::steady_time_source(),
             const contract::WallClock* wall = &contract::system_wall_clock());
  Membership(const Membership&) = delete;
  Membership& operator=(const Membership&) = delete;

  // This node's record, seen now.
  NodeRecord self() const;
  // Throws std::invalid_argument for an empty host or a zero port.
  void set_endpoint(std::string host, uint16_t port);
  // Each is true when the record changed; a change makes the record newer,
  // so gossip carries it.
  bool set_capabilities(uint32_t capabilities);
  bool set_inbound_capable(bool capable);
  bool set_path_class(PathClass path);
  // Throws std::invalid_argument past 16.
  bool set_candidates(std::vector<Candidate> candidates);
  // The hosted system's adverts. Throws std::invalid_argument for an empty
  // or over-long key, a value over 1 KiB, or a 65th advert.
  bool set_advert(const std::string& key, net::Bytes value);
  bool clear_advert(const std::string& key);

  // As gossiped, self included; a node not known reports true.
  bool inbound_capable(const identity::NodeId& id) const;

  // A record heard of: from the node's own authenticated connection
  // (`direct`) or from gossip. The newer record (by seen_unix_ms) wins; a
  // direct observation of an older one refreshes liveness only. A record
  // without an address, or this node's own, is ignored.
  void observe(NodeRecord record, bool direct = false);

  // Applies a reset newer than any held for its endpoint: drops the stale
  // members and persists the tombstone. False if it was not newer.
  bool apply_identity_reset(const IdentityReset& reset);
  std::vector<IdentityReset> identity_resets() const;

  // Self first, then every known node.
  std::vector<NodeRecord> all() const;
  // Self first, then every node heard of within dead_after.
  std::vector<NodeRecord> active() const;
  // Self, or authenticated directly within dead_after.
  bool directly_reachable(const identity::NodeId& id) const;
  // Drops every node not heard of for the horizon; how many went.
  size_t forget_unseen();
  // Every known node directly reachable, except a pair that both refuse
  // inbound connections: they cannot meet, and that is not a fault.
  bool all_known_reachable() const;

 private:
  struct Known {
    NodeRecord record;
    contract::TimePoint seen;
    std::optional<contract::TimePoint> direct_seen;
    // Wall time this node last heard of it, and the value last persisted.
    uint64_t last_heard_unix_ms = 0;
    uint64_t persisted_heard_unix_ms = 0;
  };
  struct Encoded {
    uint64_t sequence = 0;
    std::string bytes;
  };

  void load() DANNAN_REQUIRES(_mutex);
  Encoded encode_locked() DANNAN_REQUIRES(_mutex);
  void write(const Encoded& roster) DANNAN_EXCLUDES(_mutex);

  const std::chrono::milliseconds _dead_after;
  const std::chrono::milliseconds _forget_after;
  const std::filesystem::path _path;
  const contract::TimeSource* const _time;
  const contract::WallClock* const _wall;

  mutable contract::Mutex _mutex;
  NodeRecord _self DANNAN_GUARDED_BY(_mutex);
  std::unordered_map<identity::NodeId, Known, identity::NodeIdHash> _nodes DANNAN_GUARDED_BY(_mutex);
  std::unordered_map<std::string, IdentityReset> _resets DANNAN_GUARDED_BY(_mutex);
  uint64_t _sequence DANNAN_GUARDED_BY(_mutex) = 0;

  // Guards nothing but the order of writes.
  mutable contract::IoMutex _persist_mutex;
  uint64_t _written DANNAN_GUARDED_BY(_persist_mutex) = 0;
};

}  // namespace dannan::membership
