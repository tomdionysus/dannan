//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <string_view>

#include "dannan/identity/cluster_key.h"
#include "dannan/identity/node_id.h"

namespace dannan::identity {

// An exclusive lock on a state path, held for the process's life: taken
// before any state file is read, so two processes never share one. The
// file's name is protocol 23's, so a Macha process and a Dannan node on
// one path exclude each other. Throws std::runtime_error if the path is
// already in use.
class StateLock {
 public:
  explicit StateLock(const std::filesystem::path& root, std::string_view name = ".macha.lock");
  ~StateLock();
  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;

 private:
  int _fd = -1;
};

// The node's durable id, from `state`/node-id (thirty-two hex digits),
// created with a random id, owner-only and durably, on first start. Throws
// std::runtime_error for a file that is not an id.
NodeId load_or_create_node_id(const std::filesystem::path& state);

// Who this node is: the cluster key, its durable id, and this process's
// durability epoch, fresh on every start, so peers can tell a restart from
// a reconnect. Built while the state lock is held; read-only afterwards.
struct NodeIdentity {
  NodeIdentity(const std::filesystem::path& state, ClusterKey key);

  ClusterKey key;
  NodeId id;
  NodeId durability_epoch;
};

}  // namespace dannan::identity
