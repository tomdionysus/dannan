//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// A node's readiness: its startup planes, and its health after startup.

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

/// A node's health after startup (0.12.0; TODO/2026-11-23-readiness-health.md). Worst wins: the
/// order is the severity, so the health of several checks is the greatest of them.
enum class Health : uint8_t {
  /// Serving normally.
  ready = 0,
  /// Serving, short of something (a limit unenforced, a certificate near expiry): still placed.
  degraded = 1,
  /// Leaving on the operator's word: not chosen for new work, still a holder and a fallback.
  draining = 2,
  /// Not serving: starting, failed, or a check says so. Placement passes it over.
  not_ready = 3,
};
/// "ready", "degraded", "draining" or "not_ready".
const char* health_name(Health) noexcept;

/// One check's verdict, as Readiness::checks() reports it.
struct CheckState {
  /// The check's name: "dannan.clock", "louhi.runtime".
  std::string name;
  /// What it says.
  Health health = Health::ready;
  /// Why, for a person; empty when ready. For Status and telemetry, behind a caller's credential:
  /// never on an unauthenticated route, since a reason can hold a figure or a path (Lugh).
  std::string reason;
  /// When its health last changed, Unix milliseconds.
  uint64_t since_unix_ms = 0;
};

/// The advert a node's own record carries its health in: one byte, absent when ready, so a ready
/// node's record is as it was before 0.12.0.
inline constexpr const char* health_advert = "dannan.health";
/// A peer's advertised health, from its record. A record without the advert (a node before
/// 0.12.0), or with one of another width or value, reads ready: it did not say.
Health health_of(const membership::NodeRecord& node);
/// Sets `node`'s health advert to `health`, or removes it for ready.
void set_health_advert(membership::NodeRecord& node, Health health);

/// A node's readiness, as Context::readiness().
///
/// **Startup.** The control plane, and the planes whoever recovers local state declares by name
/// (a store declares its data and metadata planes), each marked ready when recovered. The node is
/// reachable before it is ready: the control plane comes up first, so peers can tell reachability
/// from readiness. phase() is what telemetry reports.
///
/// **Health** (0.12.0). After startup, health() is the worst of the operator's drain and every
/// check pushed with set(). Dannan pushes its own: dannan.clock (not_ready while the node's clock
/// is out of bounds), dannan.memory (degraded while the ledger's headroom is used up) and
/// dannan.certificate (degraded within seven days of expiry, not_ready past it). A hosted system
/// pushes its own under its own prefix. The node's record advertises its health once a worse one
/// has held for NodeConfig::health_hold, and placement demotes draining then not_ready nodes.
///
/// Why pushed, never polled: a check that does I/O (asking Docker, say) then blocks nothing of
/// Dannan's; the hosted system runs it on its own loop and reports.
///
/// Thread-safe; every member waits on nothing but its own lock, except wait_complete().
///
/// @code
/// auto& readiness = context.readiness();
/// // On the hosted system's own status loop, every ten seconds:
/// readiness.set("louhi.runtime", docker_answers ? status::Health::ready : status::Health::not_ready,
///               "docker did not answer", std::chrono::seconds(30));  // stale after 30 s
/// // Told of each change, on its executor:
/// readiness.on_change(executor, [](status::Health now) { /* rewrite a record, refuse clients */ });
/// @endcode
class Readiness {
 public:
  /// @param planes planes declared from the start.
  /// @param wall the wall clock times and expiries are read on; it must outlive this.
  explicit Readiness(std::vector<std::string> planes = {}, const contract::WallClock* wall = &contract::system_wall_clock());

  /// Declares a plane, not yet ready, for a hosted system that recovers state of its own. Callable
  /// until the node starts (close_declarations()), so a node is never ready before every plane it
  /// will have is known. Declaring one already declared does nothing.
  /// @throws std::invalid_argument for an empty name.
  /// @throws std::logic_error once the node has started.
  void declare(const std::string& plane);
  /// For a system with work after its planes: complete() then also waits for mark_complete(), so
  /// the node does not read as ready, and peers do not trust its figures, while recovery still
  /// runs. Callable until the node starts.
  /// @throws std::logic_error once the node has started.
  void hold_until_marked();
  /// The work hold_until_marked() waits for is done.
  void mark_complete();
  /// The node starting: the planes are fixed from here. The node's own call.
  void close_declarations();

  /// Whether the control plane is up. The node's own call.
  void control_plane(bool online);
  /// Whether the control plane is up.
  bool control_plane_online() const;
  /// Marks a declared plane ready.
  /// @throws std::invalid_argument for a plane never declared.
  void ready(const std::string& plane);
  /// Startup failed. The first failure is kept: later ones are usually its consequences.
  void fail(std::string error);
  /// Whether `plane` is declared and ready.
  bool has(const std::string& plane) const;
  /// Every declared plane ready, and the mark given if one is held for.
  bool complete() const;
  /// Whether fail() was called.
  bool failed() const;
  /// The first failure, or empty.
  std::string error() const;
  /// When startup completed, Unix milliseconds; 0 until it has.
  uint64_t ready_unix_ms() const;
  /// When this readiness was made, Unix milliseconds.
  uint64_t started_unix_ms() const noexcept { return _started_unix_ms; }
  /// The phase telemetry and Status report. A failed node says recovering: the wire has no state
  /// for it, and its own Status has the detail.
  Phase phase() const;
  /// Waits until startup is complete or failed, or `timeout` passes.
  /// @return whether it is complete.
  bool wait_complete(std::chrono::milliseconds timeout);

  /// A check's verdict, pushed by whoever watches the thing checked (0.12.0).
  ///
  /// Setting the same health again keeps its since_unix_ms. Waits on nothing.
  /// @param check its name; names under "dannan." are Dannan's own.
  /// @param health what it says.
  /// @param reason why, for a person; dropped when ready.
  /// @param valid_for if given, how long the verdict stands: one not set again within it reads
  ///   not_ready, "stopped reporting", so a hung probe reads as not ready by itself. Judged on
  ///   this readiness's wall clock.
  /// @throws std::invalid_argument for an empty name.
  void set(const std::string& check, Health health, std::string reason = {}, std::optional<std::chrono::milliseconds> valid_for = std::nullopt);
  /// Removes a check; one never set does nothing.
  void clear(const std::string& check);
  /// The operator's drain: draining until undone, reported among the checks as "dannan.drain".
  /// The node-program helper drains on a stop signal.
  /// @param on draining, or not.
  /// @param reason why, for a person.
  void drain(bool on, std::string reason = {});

  /// The node's health: not_ready until startup is complete with the control plane up, and after
  /// a failure; otherwise the worst of the drain and every check.
  Health health() const;
  /// Every check, the drain included, ordered by name, each with its reason and since.
  std::vector<CheckState> checks() const;

  /// Tells `handler` of each change of health(), once per change, posted to `executor`, never
  /// called inline.
  /// @return an id for cancel().
  uint64_t on_change(boost::asio::any_io_executor executor, std::function<void(Health)> handler);
  /// Stops telling a handler; a change already posted may still arrive.
  void cancel(uint64_t id);
  /// Looks again for a change no call made (a verdict expiring). The node's status step calls it.
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
