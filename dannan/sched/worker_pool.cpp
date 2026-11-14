//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/worker_pool.h"

#include <time.h>

#include <algorithm>
#include <boost/asio/post.hpp>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

#include "dannan/supervise/supervised.h"

namespace dannan::sched {

namespace {

constexpr size_t index_of(WorkClass work) noexcept { return static_cast<size_t>(work); }
constexpr WorkClass class_at(size_t index) noexcept { return static_cast<WorkClass>(index); }
constexpr bool background(WorkClass work) noexcept { return work == WorkClass::loader || work == WorkClass::speculative; }

WorkerPoolConfig checked(WorkerPoolConfig config) {
  if (config.threads == 0) throw std::invalid_argument("a worker pool has at least one thread");
  size_t reserved = 0;
  for (const auto each : config.reserve) reserved += each;
  if (reserved >= config.threads) throw std::invalid_argument("a worker pool's reserves leave no worker for the lowest class");
  return config;
}

}  // namespace

double thread_cpu_seconds() noexcept {
  timespec own{};
  if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &own) != 0) return 0.0;
  return static_cast<double>(own.tv_sec) + static_cast<double>(own.tv_nsec) / 1e9;
}

const char* refused_name(WorkerPool::Refused why) noexcept {
  switch (why) {
    case WorkerPool::Refused::none:
      return "none";
    case WorkerPool::Refused::queue_full:
      return "queue_full";
    case WorkerPool::Refused::stopped:
      return "stopped";
  }
  return "unknown";
}

const char* end_name(WorkerPool::End how) noexcept {
  switch (how) {
    case WorkerPool::End::ran:
      return "ran";
    case WorkerPool::End::faulted:
      return "faulted";
    case WorkerPool::End::past_deadline:
      return "past_deadline";
    case WorkerPool::End::cancelled:
      return "cancelled";
    case WorkerPool::End::stopped:
      return "stopped";
  }
  return "unknown";
}

bool WorkerPool::Turn::should_yield() {
  if (_context.cancelled() || _context.expired(_time->now())) return true;
  return _gate && _gate->should_yield();
}

WorkerPool::WorkerPool(WorkerPoolConfig config, ClassGates& gates, supervise::SupervisedThreads* threads, const contract::TimeSource* time,
                       ThreadCpu thread_cpu)
    : _config(checked(std::move(config))),
      _gates(gates),
      _threads(threads),
      _time(time),
      _thread_cpu(thread_cpu ? std::move(thread_cpu) : ThreadCpu(thread_cpu_seconds)) {}

WorkerPool::~WorkerPool() { stop(); }

void WorkerPool::start() {
  {
    contract::Lock lock(_mutex);
    if (_started || _stopping) return;
    _started = true;
  }
  for (size_t i = 0; i < _config.threads; ++i) {
    const auto name = _config.name + "-" + std::to_string(i);
    _workers.emplace_back([this, name](std::stop_token stop) { supervise::supervised_loop(_threads, name, stop, [this, stop] { work(stop); }); });
  }
}

void WorkerPool::stop() {
  std::vector<Pending> queued;
  {
    contract::Lock lock(_mutex);
    if (_stopping && _workers.empty()) return;
    _stopping = true;
    for (size_t i = 0; i < _queues.size(); ++i) {
      for (auto& pending : _queues[i]) {
        queued.push_back(std::move(pending));
        gone(i);
        --_stats[i].queued;
      }
      _queues[i].clear();
    }
  }
  _wake.notify_all();
  // Told before the running tasks are waited for: their callers need not wait on them.
  for (auto& pending : queued) tell(pending, End::stopped);
  for (auto& worker : _workers) worker.request_stop();
  _wake.notify_all();
  for (auto& worker : _workers)
    if (worker.joinable()) worker.join();
  _workers.clear();
}

WorkerPool::Submitted WorkerPool::submit(const WorkContext& context, Task task, boost::asio::any_io_executor executor, std::function<void(Ended)> done) {
  if (done && !executor) throw std::invalid_argument("a worker pool's completion needs an executor to be posted to");
  const auto index = index_of(context.work());
  {
    contract::Lock lock(_mutex);
    auto& stats = _stats[index];
    if (_stopping) {
      ++stats.refused_stopped;
      return {false, Refused::stopped};
    }
    if (_queues[index].size() >= _config.queue_bound[index]) {
      ++stats.refused_full;
      return {false, Refused::queue_full};
    }
    _queues[index].push_back(Pending{context, std::move(task), std::move(executor), std::move(done)});
    ++stats.accepted;
    ++stats.queued;
    _gates.note(context.work(), +1);
  }
  _wake.notify_one();
  return {true, Refused::none};
}

WorkerPool::ClassStats WorkerPool::stats(WorkClass work) const {
  contract::Lock lock(_mutex);
  return _stats[index_of(work)];
}

void WorkerPool::tell(Pending& pending, End how, std::string fault) {
  if (!pending.done) return;
  boost::asio::post(pending.executor, [done = std::move(pending.done), how, fault = std::move(fault)] { done(Ended{how, fault}); });
}

void WorkerPool::gone(size_t index) { _gates.note(class_at(index), -1); }

void WorkerPool::sweep(contract::TimePoint now, std::vector<std::pair<Pending, End>>& ended) {
  for (size_t i = 0; i < _queues.size(); ++i) {
    auto& queue = _queues[i];
    for (auto it = queue.begin(); it != queue.end();) {
      const bool cancelled = it->context.cancelled();
      if (!cancelled && !it->context.expired(now)) {
        ++it;
        continue;
      }
      ended.emplace_back(std::move(*it), cancelled ? End::cancelled : End::past_deadline);
      it = queue.erase(it);
      --_stats[i].queued;
      ++_stats[i].dropped;
      gone(i);
    }
  }
}

WorkerPool::Picked WorkerPool::pick(contract::TimePoint now, contract::TimePoint& wake) {
  for (size_t i = 0; i < _queues.size(); ++i) {
    if (_queues[i].empty()) continue;
    const auto cap = _config.max_running[i] ? _config.max_running[i] : _config.threads;
    if (_stats[i].running >= cap) continue;
    // What must stay free for the classes above, less what they already hold.
    size_t held_back = 0;
    for (size_t above = 0; above < i; ++above)
      if (_config.reserve[above] > _stats[above].running) held_back += _config.reserve[above] - _stats[above].running;
    const size_t free_after = _config.threads - _running - 1;
    if (free_after < held_back) continue;
    const auto which = class_at(i);
    if (background(which)) {
      auto& gate = _gates.gate(which);
      if (!gate.may_start()) {
        // A rest ends at a time; a burst another worker holds ends when it finishes.
        if (const auto rest = gate.rest(); rest.count() > 0) {
          const auto until = now + rest;
          if (wake == contract::TimePoint{} || until < wake) wake = until;
        }
        continue;
      }
    }
    return {true, i};
  }
  return {};
}

void WorkerPool::work(std::stop_token stop) {
  for (;;) {
    std::optional<Pending> job;
    size_t index = 0;
    std::vector<std::pair<Pending, End>> ended;
    {
      contract::Lock lock(_mutex);
      for (;;) {
        if (_stopping || stop.stop_requested()) break;
        const auto now = _time->now();
        sweep(now, ended);
        contract::TimePoint wake{};
        if (const auto picked = pick(now, wake); picked.found) {
          index = picked.index;
          job = std::move(_queues[index].front());
          _queues[index].pop_front();
          --_stats[index].queued;
          ++_stats[index].running;
          ++_running;
          break;
        }
        // Dropped work is told outside the lock before looking again.
        if (!ended.empty()) break;
        if (wake != contract::TimePoint{}) {
          const auto remaining = std::max(std::chrono::ceil<std::chrono::milliseconds>(wake - now), std::chrono::milliseconds(1));
          _wake.wait_for(lock.native(), _time->wake_within(remaining));
        } else {
          _wake.wait(lock.native());
        }
      }
    }
    for (auto& [pending, how] : ended) tell(pending, how);
    if (!job) {
      contract::Lock lock(_mutex);
      if (_stopping || stop.stop_requested()) return;
      continue;
    }
    const auto which = class_at(index);
    ClassGate* gate = background(which) ? &_gates.gate(which) : nullptr;
    if (gate) gate->started();
    std::string fault;
    bool faulted = false;
    const double cpu_before = _thread_cpu();
    {
      Turn turn(job->context, gate, _time);
      try {
        job->task(turn);
      } catch (const std::exception& error) {
        faulted = true;
        fault = error.what();
      } catch (...) {
        faulted = true;
        fault = "unknown exception";
      }
    }
    std::chrono::milliseconds rested{0};
    if (gate) {
      gate->finished();
      rested = gate->charge_and_rest(std::max(0.0, _thread_cpu() - cpu_before));
    }
    {
      contract::Lock lock(_mutex);
      auto& stats = _stats[index];
      --stats.running;
      --_running;
      ++(faulted ? stats.faulted : stats.ran);
      stats.rested += rested;
      gone(index);
    }
    // A class's end can free a worker, a cap or a burst for any waiting class. A
    // background class skipped because this worker held its burst (rest zero, so no
    // wake time) does not hang on this: this worker picks again before it sleeps, and
    // either takes it or sets the wake for the rest its burst earned. The rest of the
    // waiters may be anywhere, so all are woken.
    _wake.notify_all();
    tell(*job, faulted ? End::faulted : End::ran, std::move(fault));
  }
}

}  // namespace dannan::sched
