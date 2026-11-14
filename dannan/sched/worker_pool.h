//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/sched/class_gate.h"
#include "dannan/sched/work.h"

namespace dannan::supervise {
class SupervisedThreads;
}

namespace dannan::sched {

// The calling thread's CPU seconds since some fixed point; the real one reads
// CLOCK_THREAD_CPUTIME_ID.
using ThreadCpu = std::function<double()>;
double thread_cpu_seconds() noexcept;

// Indexed by WorkClass: control, foreground, loader, speculative.
template <typename T>
using PerClass = std::array<T, 4>;

struct WorkerPoolConfig {
  // Its threads, named "<name>-<n>".
  size_t threads = 4;
  std::string name = "work";
  // How many of a class's tasks run at once (zero: as many as there are threads).
  PerClass<size_t> max_running{0, 0, 0, 0};
  // How many of a class's tasks may wait; one more is refused.
  PerClass<size_t> queue_bound{1024, 1024, 1024, 1024};
  // Workers held back for each class, so work below it, however much is blocked,
  // never takes the last of them. Fewer in all than the threads.
  PerClass<size_t> reserve{1, 1, 0, 0};
};

// Supervised threads running tasks by work class (P4): control, foreground,
// loader, speculative (laws 1 to 4), first in first out within a class. Loader
// and speculative tasks run through their class's gate, shared with every other
// pool and loop of the node, and rest by the CPU they use. A hosted system may run
// a pool of its own beside the node's, under the node's supervision and gates.
//
// A task may block only within its WorkContext's declared waits and before its
// deadline: one that would wait on a peer for long is a callback, not a held worker.
// Thread-safe.
class WorkerPool {
 public:
  enum class Refused : uint8_t { none, queue_full, stopped };
  struct Submitted {
    bool accepted = false;
    Refused why = Refused::none;
  };
  enum class End : uint8_t { ran, faulted, past_deadline, cancelled, stopped };
  struct Ended {
    End how = End::ran;
    // What a faulted task threw.
    std::string fault;
  };

  // What a running task may ask of its turn.
  class Turn {
   public:
    const WorkContext& context() const noexcept { return _context; }
    // True when a background task should end its turn at this operation boundary:
    // its gate's slice is spent while the work above contends, or it is cancelled
    // or past its deadline. Never true for control or foreground unless those.
    bool should_yield();

   private:
    friend class WorkerPool;
    Turn(const WorkContext& context, ClassGate* gate, const contract::TimeSource* time) : _context(context), _gate(gate), _time(time) {}
    const WorkContext& _context;
    ClassGate* const _gate;
    const contract::TimeSource* const _time;
  };
  using Task = std::function<void(Turn&)>;

  struct ClassStats {
    uint64_t queued = 0;
    uint64_t running = 0;
    uint64_t accepted = 0;
    uint64_t refused_full = 0;
    uint64_t refused_stopped = 0;
    uint64_t dropped = 0;
    uint64_t ran = 0;
    uint64_t faulted = 0;
    std::chrono::milliseconds rested{0};
  };

  // `threads` supervises its threads (none: an unrecorded supervisor). Throws
  // std::invalid_argument for no threads or reserves that leave none.
  WorkerPool(WorkerPoolConfig config, ClassGates& gates, supervise::SupervisedThreads* threads = nullptr,
             const contract::TimeSource* time = &contract::steady_time_source(), ThreadCpu thread_cpu = thread_cpu_seconds);
  ~WorkerPool();
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Starts the threads; tasks submitted before wait for it.
  void start();
  // Refuses what comes next, ends every queued task as stopped, and joins the
  // threads once the running tasks return. Waits on them.
  void stop();

  // Queues `task` under `context`'s class. Never waits. `done`, if given, is posted
  // to `executor` once it has ended: run, faulted, or dropped unrun because its
  // deadline passed or it was cancelled while queued, or the pool stopped. Throws
  // std::invalid_argument for a `done` without an executor.
  Submitted submit(const WorkContext& context, Task task, boost::asio::any_io_executor executor = {}, std::function<void(Ended)> done = {});

  ClassStats stats(WorkClass work) const;
  const WorkerPoolConfig& config() const noexcept { return _config; }

 private:
  struct Pending {
    WorkContext context;
    Task task;
    boost::asio::any_io_executor executor;
    std::function<void(Ended)> done;
  };
  struct Picked {
    bool found = false;
    size_t index = 0;
  };

  void work(std::stop_token stop);
  // The class to run next, or none; `wake` is when a rest ends, if one holds a class back.
  Picked pick(contract::TimePoint now, contract::TimePoint& wake) DANNAN_REQUIRES(_mutex);
  // Takes the tasks whose deadline has passed or which were cancelled out of the queues.
  void sweep(contract::TimePoint now, std::vector<std::pair<Pending, End>>& ended) DANNAN_REQUIRES(_mutex);
  // A task that is no longer queued or running.
  void gone(size_t index) DANNAN_REQUIRES(_mutex);
  static void tell(Pending& pending, End how, std::string fault = {});

  const WorkerPoolConfig _config;
  ClassGates& _gates;
  supervise::SupervisedThreads* const _threads;
  const contract::TimeSource* const _time;
  const ThreadCpu _thread_cpu;

  mutable contract::Mutex _mutex;
  std::condition_variable _wake;
  PerClass<std::deque<Pending>> _queues DANNAN_GUARDED_BY(_mutex);
  PerClass<ClassStats> _stats DANNAN_GUARDED_BY(_mutex);
  size_t _running DANNAN_GUARDED_BY(_mutex) = 0;
  bool _started DANNAN_GUARDED_BY(_mutex) = false;
  bool _stopping DANNAN_GUARDED_BY(_mutex) = false;
  std::vector<std::jthread> _workers;
};

const char* refused_name(WorkerPool::Refused) noexcept;
const char* end_name(WorkerPool::End) noexcept;

}  // namespace dannan::sched
