//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/node_id.h"
#include "dannan/membership/membership.h"

namespace dannan::membership {

// Which side of a split this node is on, read against the roster as last
// agreed (P2; Tom, 2026-11-13). A majority only with more than half of the
// agreed roster reachable, this node counted: an even split is a minority, so
// two halves never both act as the majority. One rule for every hosted system.
struct Partition {
  bool majority = false;
  // The agreed roster, this node included, and how many of it are reachable
  // (active: heard of within dead_after, directly or by gossip).
  size_t members = 0;
  size_t reachable = 0;
  // The agreed members not reachable now, the evidence.
  std::vector<identity::NodeId> unreachable;
  // When it was read, on the wall clock.
  uint64_t unix_ms = 0;
  bool operator==(const Partition&) const = default;
};

// The partition reading of `members` (self among them) with `active` the
// nodes reachable now; a node active but not a member counts for nothing.
Partition read_partition(const std::set<identity::NodeId>& members, const std::set<identity::NodeId>& active, uint64_t unix_ms);

// The roster as last agreed: the nodes a partition reading counts. A node
// joins it when it is seen to join; only an operator's retire (or an
// identity reset of its id) takes it out, never absence, so a split cannot
// shrink it until each side calls itself the majority. A retired node that
// is seen to join again, a restart included, is a member again. Persisted
// under the state path (DNNAGR01: the ids). Joins are held in memory and
// written by flush(), off the io threads; a retire is written at once.
// Thread-safe.
class AgreedRoster {
 public:
  // Throws std::runtime_error if the file exists and cannot be read. An empty
  // path persists nothing.
  AgreedRoster(identity::NodeId self, std::filesystem::path path);
  AgreedRoster(const AgreedRoster&) = delete;
  AgreedRoster& operator=(const AgreedRoster&) = delete;

  // Whether a file was there to load: when not, the node seeds the roster
  // from the nodes it knows.
  bool found() const noexcept { return _found; }
  // Adds those not members; true if any was. Waits on nothing.
  bool join(const std::vector<identity::NodeId>& ids);
  // Takes `id` out and writes at once; false if it was not a member. Throws
  // std::invalid_argument for this node. Waits on the state device.
  bool retire(const identity::NodeId& id);
  std::set<identity::NodeId> members() const;
  // Writes what joins left unwritten. Waits on the state device; throws if it
  // cannot, leaving it still to be written.
  void flush();

 private:
  struct Encoded {
    uint64_t sequence = 0;
    std::string bytes;
  };
  Encoded encode_locked() DANNAN_REQUIRES(_mutex);
  void write(const Encoded& encoded) DANNAN_EXCLUDES(_mutex);

  const identity::NodeId _self;
  const std::filesystem::path _path;
  bool _found = false;
  // Never held across the write: encoded under it, written by the sequenced
  // file outside it, so a reader never waits on the disk.
  mutable contract::Mutex _mutex;
  std::set<identity::NodeId> _members DANNAN_GUARDED_BY(_mutex);
  uint64_t _sequence DANNAN_GUARDED_BY(_mutex) = 0;
  bool _unwritten DANNAN_GUARDED_BY(_mutex) = false;
  SequencedFile _file{_path};
};

}  // namespace dannan::membership
