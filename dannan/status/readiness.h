//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/telemetry.h"

namespace dannan::status {

// A node's readiness: the control plane, and the planes whoever recovers
// local state declares by name (a store names data_storage, metadata, and
// so on). The node is reachable before it is ready: the control plane comes
// up first so peers can tell reachability from readiness. Thread-safe.
class Readiness {
 public:
  explicit Readiness(std::vector<std::string> planes = {}, const contract::WallClock* wall = &contract::system_wall_clock());

  // Declares a plane, not yet ready, for a hosted system that recovers
  // state of its own: callable until the node starts (close_declarations),
  // so a node is never ready before every plane it will have is known.
  // Declaring one already declared does nothing. Throws
  // std::invalid_argument for an empty name, std::logic_error once closed.
  void declare(const std::string& plane);
  // For a system with work after its planes: complete() then also waits for
  // mark_complete(), so the node does not read as ready, and peers do not trust
  // its figures, while recovery still runs. Callable until the node starts.
  void hold_until_marked();
  void mark_complete();
  // The node starting: the planes are fixed from here.
  void close_declarations();

  void control_plane(bool online);
  bool control_plane_online() const;
  // Throws std::invalid_argument for a plane not declared.
  void ready(const std::string& plane);
  // Keeps the first failure: later ones are usually its consequences.
  void fail(std::string error);
  bool has(const std::string& plane) const;
  // Every declared plane ready.
  bool complete() const;
  bool failed() const;
  std::string error() const;
  uint64_t ready_unix_ms() const;
  uint64_t started_unix_ms() const noexcept { return _started_unix_ms; }
  // The phase telemetry and Status report: a failed node says recovering
  // (the wire has no state for it; its own Status has the detail).
  Phase phase() const;
  // Waits until complete or failed, or the timeout; whether complete.
  bool wait_complete(std::chrono::milliseconds timeout);

 private:
  bool complete_locked() const DANNAN_REQUIRES(_mutex);

  const contract::WallClock* const _wall;
  const uint64_t _started_unix_ms;
  mutable contract::Mutex _mutex;
  std::condition_variable _changed;
  std::map<std::string, bool> _planes DANNAN_GUARDED_BY(_mutex);
  bool _online DANNAN_GUARDED_BY(_mutex) = false;
  bool _closed DANNAN_GUARDED_BY(_mutex) = false;
  bool _failed DANNAN_GUARDED_BY(_mutex) = false;
  bool _held DANNAN_GUARDED_BY(_mutex) = false;
  bool _marked DANNAN_GUARDED_BY(_mutex) = false;
  std::string _error DANNAN_GUARDED_BY(_mutex);
  uint64_t _ready_unix_ms DANNAN_GUARDED_BY(_mutex) = 0;
};

}  // namespace dannan::status
