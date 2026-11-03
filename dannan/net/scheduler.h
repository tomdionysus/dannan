//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/message.h"
#include "dannan/net/session.h"

namespace dannan::net {

// The server's executors and their bounds. Each class of work has its own
// workers, so no class waits behind another's (laws 1 to 4).
struct ServerLimits {
  // Health and membership, never behind storage-backed control work.
  size_t fast_control_workers = 2;
  size_t control_workers = 2;
  // One job per peer at a time; more workers serve more peers at once.
  size_t ordered_workers = 1;
  size_t ordered_pending_jobs = 64;
  size_t ordered_pending_bytes = 256ull * 1024 * 1024;
  size_t data_workers = 8;
  // DATA workers only foreground work may take, so lower classes never hold
  // every one (law 2).
  size_t foreground_data_reserve = 2;
  size_t fast_control_pending_bytes = 1ull * 1024 * 1024;
  size_t control_pending_bytes = 16ull * 1024 * 1024;
  size_t data_pending_bytes = 64ull * 1024 * 1024;
  // Jobs per queue, beside the byte bounds.
  size_t max_queued_jobs = max_pending_replies;
};

struct Timing {
  uint64_t requests = 0;
  uint64_t queue_wait_us_total = 0;
  uint64_t queue_wait_us_max = 0;
  uint64_t handler_us_total = 0;
  uint64_t handler_us_max = 0;
};

struct WorkStats {
  size_t ordered_pending_jobs = 0;
  size_t ordered_pending_bytes = 0;
  size_t ordered_active_jobs = 0;
  uint64_t ordered_rejected = 0;
  size_t fast_control_pending_bytes = 0;
  size_t control_pending_bytes = 0;
  size_t data_pending_bytes = 0;
  uint64_t rejected = 0;
  // Sessions the server holds, filled by the server: proof that finished
  // ones are reaped. Not part of Status.
  size_t sessions = 0;
  std::map<sched::FrameType, Timing> frame_timings;
  std::map<MessageType, Timing> message_timings;
};

// Answers a request on a worker. May block; an exception answers 500.
using RequestHandler = std::function<Message(const membership::NodeRecord& peer, sched::FrameType frame, const Message& request)>;

// Queues peers' requests by class and runs them on bounded per-class
// workers. A request that cannot be queued is answered at once with a 503
// to try later and counted. All public members are thread-safe.
class RequestScheduler {
 public:
  RequestScheduler(std::shared_ptr<const RouteTable> routes, ServerLimits limits, RequestHandler handler, sched::RetainedMemoryLedger* memory = nullptr,
                   const contract::TimeSource* time = &contract::steady_time_source());
  ~RequestScheduler();
  RequestScheduler(const RequestScheduler&) = delete;
  RequestScheduler& operator=(const RequestScheduler&) = delete;

  // Queues `frame` from `peer`; `reply` is empty for a notification. Waits
  // on nothing.
  void submit(membership::NodeRecord peer, Frame frame, Reply reply);
  // The peer promoted, or cancelled, a request still queued here.
  void promote(const identity::NodeId& peer, uint64_t request_id, sched::FrameType frame);
  void cancel(const identity::NodeId& peer, uint64_t request_id);
  // Finishes running jobs and drops the queues; joins the workers. Waits on
  // the handlers in flight.
  void stop();

  WorkStats stats() const;
  // Whether stop() has begun: nothing queued after this runs.
  bool stopping() const;
  const RouteTable& routes() const noexcept { return *_routes; }

 private:
  enum class Queue { fast, control, ordered, foreground, read_ahead, loader, speculative };
  struct Job {
    membership::NodeRecord peer;
    Frame frame;
    Reply reply;
    contract::TimePoint queued;
    sched::RetainedMemoryLedger::Lease memory;
  };

  static Queue queue_for(sched::FrameType frame) noexcept;
  std::deque<Job>& queue(Queue which) DANNAN_REQUIRES(_mutex);
  size_t* bytes_for(Queue which) DANNAN_REQUIRES(_mutex);
  bool data_ready_locked() const DANNAN_REQUIRES(_mutex);
  void run(Job job);
  void fast_loop(std::stop_token stop);
  void control_loop(std::stop_token stop);
  void ordered_loop(std::stop_token stop);
  void data_loop(std::stop_token stop);

  const std::shared_ptr<const RouteTable> _routes;
  const ServerLimits _limits;
  const RequestHandler _handler;
  sched::RetainedMemoryLedger* const _memory;
  const contract::TimeSource* const _time;

  mutable contract::Mutex _mutex;
  std::condition_variable _changed;
  std::deque<Job> _fast DANNAN_GUARDED_BY(_mutex);
  std::deque<Job> _control DANNAN_GUARDED_BY(_mutex);
  std::deque<Job> _ordered DANNAN_GUARDED_BY(_mutex);
  std::deque<Job> _foreground DANNAN_GUARDED_BY(_mutex);
  std::deque<Job> _read_ahead DANNAN_GUARDED_BY(_mutex);
  std::deque<Job> _loader DANNAN_GUARDED_BY(_mutex);
  std::deque<Job> _speculative DANNAN_GUARDED_BY(_mutex);
  size_t _fast_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  size_t _control_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  size_t _ordered_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  size_t _data_bytes DANNAN_GUARDED_BY(_mutex) = 0;
  std::set<identity::NodeId> _ordered_active DANNAN_GUARDED_BY(_mutex);
  size_t _nonforeground_active DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _rejected DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _ordered_rejected DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopping DANNAN_GUARDED_BY(_mutex) = false;

  // Statistics only; never held across anything.
  mutable contract::Mutex _timing_mutex;
  std::map<sched::FrameType, Timing> _frame_timings DANNAN_GUARDED_BY(_timing_mutex);
  std::map<MessageType, Timing> _message_timings DANNAN_GUARDED_BY(_timing_mutex);

  std::vector<std::jthread> _workers;
};

}  // namespace dannan::net
