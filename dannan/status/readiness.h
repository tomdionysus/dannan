//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/membership/node_record.h"
#include "dannan/status/telemetry.h"

namespace dannan::status {

// A node's health after startup (TODO/2026-11-23-readiness-health.md, item 7). Worst wins: the
// order is the severity.
enum class Health : uint8_t { ready = 0, degraded = 1, draining = 2, not_ready = 3 };
const char* health_name(Health) noexcept;

// One check's verdict, as checks() reports it.
struct CheckState {
  std::string name;
  Health health = Health::ready;
  // Empty when ready. For Status and telemetry, behind a caller's credential: never on an
  // unauthenticated route (Lugh), since a reason can hold a figure or a path.
  std::string reason;
  // When its health last changed.
  uint64_t since_unix_ms = 0;
};

// The advert a node's own record carries its health in: one byte, absent when ready, so a ready
// node's record is as it was before 0.12.0.
inline constexpr const char* health_advert = "dannan.health";
// A peer's advertised health. A record without the advert, or with one of another width or
// value, reads ready: it did not say.
Health health_of(const membership::NodeRecord& node);
// Sets `node`'s advert to `health`, or removes it for ready.
void set_health_advert(membership::NodeRecord& node, Health health);

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

  // A check's verdict, pushed by whoever watches the thing checked: never a callback Dannan runs,
  // so a check that does I/O blocks nothing of Dannan's. With `valid_for`, a verdict not set again
  // within it reads not_ready ("stopped reporting"), judged on this readiness's wall clock.
  // Setting the same health again keeps its since. Names under "dannan." are Dannan's own. Throws
  // std::invalid_argument for an empty name. Waits on nothing.
  void set(const std::string& check, Health health, std::string reason = {}, std::optional<std::chrono::milliseconds> valid_for = std::nullopt);
  void clear(const std::string& check);
  // The operator's drain: draining until undone. Reported among the checks as "dannan.drain".
  void drain(bool on, std::string reason = {});

  // not_ready until startup is complete with the control plane up, and after a failure; else the
  // worst of the drain and every check.
  Health health() const;
  // Every check, the drain included, ordered by name.
  std::vector<CheckState> checks() const;

  // Each change of health() is posted to `executor`, never called inline, once per change. The
  // id cancels it.
  uint64_t on_change(boost::asio::any_io_executor executor, std::function<void(Health)> handler);
  void cancel(uint64_t id);
  // Looks again for a change no call made (a verdict expiring): the node's status step calls it.
  void evaluate();

 private:
  struct Check {
    Health health = Health::ready;
    std::string reason;
    uint64_t since_unix_ms = 0;
    // Zero: no expiry.
    uint64_t valid_until_unix_ms = 0;
  };
  struct Watcher {
    boost::asio::any_io_executor executor;
    std::function<void(Health)> handler;
  };

  bool complete_locked() const DANNAN_REQUIRES(_mutex);
  Health health_locked(uint64_t now) const DANNAN_REQUIRES(_mutex);
  // Recomputes health and posts it to every watcher if it changed.
  void changed_locked() DANNAN_REQUIRES(_mutex);
  void set_locked(const std::string& check, Health health, std::string reason, uint64_t valid_until) DANNAN_REQUIRES(_mutex);

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
  std::map<std::string, Check> _checks DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, Watcher> _watchers DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_watcher DANNAN_GUARDED_BY(_mutex) = 1;
  // The health last told to watchers.
  Health _told DANNAN_GUARDED_BY(_mutex) = Health::not_ready;
};

}  // namespace dannan::status
