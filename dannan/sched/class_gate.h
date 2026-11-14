//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/duty_share.h"
#include "dannan/sched/work.h"

namespace dannan::sched {

// The process's CPU seconds since some fixed point; the real one reads std::clock().
using ProcessCpu = std::function<double()>;
double process_cpu_seconds() noexcept;

struct GateConfig {
  // Foreground (and, for speculative work, loader) against this class: 95:5 gives
  // this class a twentieth of the time while they contend. Both at least 1.
  uint32_t higher_weight = 95;
  uint32_t own_weight = 5;
  std::chrono::milliseconds slice{25};
  // The share of a core the rest of the process may use before this class loses
  // budget (Macha's maintenance cpu_target), over windows of `cpu_window`.
  double cpu_target = 0.5;
  std::chrono::milliseconds cpu_window{1000};
  // The least the scale falls to: paced, never stopped.
  double min_scale = 0.05;
};

// One background class's pacing on a node (P4): the loop that holds it and the
// pools that run its tasks draw on the one share, so the class never takes more
// than its weight however many forms it runs in.
//
// It is a DutyShare against the work above it, contended while any is queued or
// running in a pool (the pools say so) or the hosted system's reading says so; and
// a CPU scale, the process's load less what this class's own work used, against
// the target. A loop calls started(), should_yield() at each operation boundary
// and finished(), and reads cpu_scale() for its own credit, charging what its
// turns used. A pool also adds the CPU rest its tasks earn. Thread-safe; waits on
// nothing but its own lock and the hosted reading.
class ClassGate {
 public:
  // Throws std::invalid_argument for control or foreground, which are never paced,
  // and for a config DutyShare refuses or a target outside (0, 1].
  ClassGate(WorkClass work, GateConfig config = {}, const contract::TimeSource* time = &contract::steady_time_source(),
            ProcessCpu process_cpu = process_cpu_seconds);

  WorkClass work() const noexcept { return _work; }

  // The hosted system's reading of whether the work above contends, or'd with the
  // pools' own. Called on whichever thread asks (a pool thread at each pick, a loop
  // at each boundary): it must not block, nor take a lock a task or the loop can
  // hold. May be set, replaced or cleared (empty) at any time, the node running
  // (Macha's review: its reading exists only once its store recovers). Until
  // set_contended returns, the old reading may still be called. None: only the pools'.
  void set_contended(std::function<bool()> reading);
  bool contended() const;
  // A pool's word that work above this class was queued (+1) or has ended (-1).
  void note_higher(int delta) noexcept;

  // The loop's form, DutyShare's, read against contended() now.
  bool may_start();
  void started(bool serving = true);
  void serving();
  bool should_yield();
  // The rest the burst earned: zero uncontended, or while other workers run.
  std::chrono::milliseconds finished();
  // How long until this class may start again: the share's rest and the CPU rest,
  // whichever ends later; zero now.
  std::chrono::milliseconds rest();

  // The fraction of its budget this class keeps, at most once a cpu_window fresh.
  double cpu_scale();
  // CPU seconds this class's own work used, taken out of the load it is paced by.
  void charge(double cpu_seconds);
  // A task that used `cpu_seconds` earns a rest of cpu_seconds x (1/scale - 1)
  // before the class's next task; what it returns.
  std::chrono::milliseconds charge_and_rest(double cpu_seconds);

 private:
  double scale_locked(contract::TimePoint now) DANNAN_REQUIRES(_mutex);

  const WorkClass _work;
  const GateConfig _config;
  const contract::TimeSource* const _time;
  const ProcessCpu _process_cpu;
  DutyShare _share;
  // Copied out under its lock and called outside it.
  mutable contract::Mutex _hosted_mutex;
  std::shared_ptr<const std::function<bool()>> _hosted DANNAN_GUARDED_BY(_hosted_mutex);
  std::atomic<int64_t> _higher{0};

  contract::Mutex _mutex;
  bool _sampled DANNAN_GUARDED_BY(_mutex) = false;
  contract::TimePoint _sampled_at DANNAN_GUARDED_BY(_mutex){};
  double _process_at DANNAN_GUARDED_BY(_mutex) = 0.0;
  double _own DANNAN_GUARDED_BY(_mutex) = 0.0;
  double _own_at DANNAN_GUARDED_BY(_mutex) = 0.0;
  double _scale DANNAN_GUARDED_BY(_mutex) = 1.0;
  contract::TimePoint _cpu_rest_until DANNAN_GUARDED_BY(_mutex){};
};

// A node's gates: one for loader work, one for speculative.
class ClassGates {
 public:
  explicit ClassGates(GateConfig loader = {}, GateConfig speculative = {}, const contract::TimeSource* time = &contract::steady_time_source(),
                      ProcessCpu process_cpu = process_cpu_seconds);
  // Throws std::invalid_argument for control or foreground.
  ClassGate& gate(WorkClass work);
  // Work of `work` queued (+1) or ended (-1): told to every gate it contends with.
  void note(WorkClass work, int delta) noexcept;

 private:
  ClassGate _loader;
  ClassGate _speculative;
};

}  // namespace dannan::sched
