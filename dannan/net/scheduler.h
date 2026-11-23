//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <atomic>
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
#include "dannan/status/log.h"
#include "dannan/supervise/supervised.h"

/// @file
/// The server's request scheduler: per-class workers and bounds, handlers, and answers given later.

namespace dannan::net {

/// The server's executors and their bounds, in NodeConfig. Each class of work has its own workers,
/// so no class waits behind another's (laws 1 to 4).
struct ServerLimits {
  /// Workers for health and membership, never behind storage-backed control work.
  size_t fast_control_workers = 2;
  /// Workers for the rest of CONTROL.
  size_t control_workers = 2;
  /// Workers for ordered types: one job per peer at a time, so more workers serve more peers at
  /// once.
  size_t ordered_workers = 1;
  /// Ordered jobs queued at most; a quarter is kept for control-class work.
  size_t ordered_pending_jobs = 64;
  /// Ordered bytes queued at most; a quarter is kept for control-class work.
  size_t ordered_pending_bytes = 256ull * 1024 * 1024;
  /// Workers for DATA.
  size_t data_workers = 8;
  /// DATA workers only foreground work may take, so lower classes never hold every one (law 2).
  size_t foreground_data_reserve = 2;
  /// Fast-control bytes queued at most.
  size_t fast_control_pending_bytes = 1ull * 1024 * 1024;
  /// Control bytes queued at most.
  size_t control_pending_bytes = 16ull * 1024 * 1024;
  /// DATA bytes queued at most.
  size_t data_pending_bytes = 64ull * 1024 * 1024;
  /// Jobs per queue, beside the byte bounds.
  size_t max_queued_jobs = max_pending_replies;
  /// Requests of one work class running or answered later (Handled::deferred()) at once; past it
  /// the class's next request is refused, a 503 to try later.
  size_t max_deferred = 4096;
  /// The most of those one peer may hold (0.12.0; Macha): 0 is a quarter of max_deferred, at least
  /// one, so no one peer takes a class's whole allowance.
  size_t max_deferred_per_peer = 0;
};

/// How long requests waited and ran, per frame type or message type.
struct Timing {
  /// Requests handled.
  uint64_t requests = 0;
  /// Of those, the handler failed.
  uint64_t failed = 0;
  /// Of those, the handler did not recognise.
  uint64_t unrecognised = 0;
  /// Microseconds queued, in total.
  uint64_t queue_wait_us_total = 0;
  /// Microseconds queued, at most.
  uint64_t queue_wait_us_max = 0;
  /// Microseconds in the handler, in total.
  uint64_t handler_us_total = 0;
  /// Microseconds in the handler, at most.
  uint64_t handler_us_max = 0;
  /// Of the requests, those whose handler answered later.
  uint64_t deferred = 0;
};

/// The scheduler's state and counts, for Status.
struct WorkStats {
  /// Ordered jobs queued now.
  size_t ordered_pending_jobs = 0;
  /// Ordered bytes queued now.
  size_t ordered_pending_bytes = 0;
  /// Ordered jobs running now.
  size_t ordered_active_jobs = 0;
  /// Ordered requests refused, full.
  uint64_t ordered_rejected = 0;
  /// Fast-control bytes queued now.
  size_t fast_control_pending_bytes = 0;
  /// Control bytes queued now.
  size_t control_pending_bytes = 0;
  /// DATA bytes queued now.
  size_t data_pending_bytes = 0;
  /// Other requests refused: a queue full, or a deferral bound reached.
  uint64_t rejected = 0;
  /// Sessions the server holds, filled by the server: proof that finished ones are reaped. Not part
  /// of Status.
  size_t sessions = 0;
  /// Timings per frame type.
  std::map<sched::FrameType, Timing> frame_timings;
  /// Timings per message type.
  std::map<MessageType, Timing> message_timings;
};

/// What a handler did with a request. Each kind is counted per type.
class Handled {
 public:
  /// The four things a handler can do.
  enum class Outcome : uint8_t {
    /// Answered: the reply (ignored for a notification).
    processed,
    /// Refused: the refusal Dannan sends.
    failed,
    /// Not this handler's type: Dannan answers 501 itself.
    unrecognised,
    /// The handler took the request's Answer and gives it later.
    deferred,
  };

  /// Answered with `reply`; an empty ok by default.
  static Handled processed(Message reply = {message::ok, {}, {}}) { return Handled(Outcome::processed, std::move(reply)); }
  /// Refused: Dannan sends an error reply with `status` (HTTP-like: 400, 404, 503), a short
  /// machine-readable `code`, and `why` for a person.
  static Handled failed(uint16_t status, std::string_view code, std::string_view why) { return Handled(Outcome::failed, error_reply(status, code, why)); }
  /// Not this handler's type: Dannan answers 501.
  static Handled unrecognised() { return Handled(Outcome::unrecognised, {}); }
  /// The handler moved the Answer out of the call and gives it later.
  static Handled deferred() { return Handled(Outcome::deferred, {}); }

  /// Which of the four.
  Outcome outcome() const noexcept { return _outcome; }
  /// The message to send back: the reply, or the refusal.
  const Message& reply() const noexcept { return _reply; }
  /// The message to send back, moved out.
  Message take_reply() noexcept { return std::move(_reply); }

 private:
  Handled(Outcome outcome, Message reply) : _outcome(outcome), _reply(std::move(reply)) {}
  Outcome _outcome;
  Message _reply;
};

/// The way to answer one request later, from any thread (P8: a group commit answers from its
/// committer, holding no worker while it waits).
///
/// A handler that returns Handled::deferred() has moved it out of the call. Given once; later
/// calls do nothing. Dropped ungiven, it refuses the call (500 "abandoned"), so the caller never
/// waits out its deadline for it. Until given it counts against its class's
/// ServerLimits::max_deferred, and its peer's share of that, and keeps the request's bytes charged.
/// For a notification giving it sends nothing; after the session closes it goes nowhere.
/// Thread-safe to give; one owner.
///
/// @code
/// routes.bind_deferred(put, [&](const auto& peer, auto frame, net::Message request, net::Answer& answer) {
///   if (!answer.hold(expected_reply_bytes)) return net::Handled::deferred();  // answered 503
///   committer.enqueue(std::move(request), std::move(answer));  // answered when it commits
///   return net::Handled::deferred();
/// });
/// @endcode
class Answer {
 public:
  /// Holds `bytes` for the answer while it waits (0.12.0; Macha: a deferring handler knows its
  /// reply's size when it parks), charged to the node's retained-memory ledger under the request's
  /// work class as MemoryOwner::deferred_reply, and released when the answer is given or dropped.
  /// Holds add up. An answer with no ledger behind it (Answer::to()) holds without a charge.
  /// @return true when held; false when the ledger refused, and then the answer has been given: a
  ///   503 "transport_busy" to try later.
  bool hold(size_t bytes);
  /// An empty answer: one already given.
  Answer() noexcept;
  /// Moves the answer; the one moved from is empty.
  Answer(Answer&& other) noexcept;
  /// Moves the answer in; one held here and not given is refused first, as a drop is.
  Answer& operator=(Answer&& other) noexcept;
  /// Refuses the call (500 "abandoned") if not given.
  ~Answer();
  /// True until given, dropped or moved from.
  explicit operator bool() const noexcept { return static_cast<bool>(_state); }
  /// Gives the answer: `reply` is sent, and the class's place and the bytes held are let go.
  void operator()(Message reply);
  /// Gives a refusal, as Handled::failed() would.
  void fail(uint16_t status, std::string_view code, std::string_view why);
  /// An answer that sends `reply` and holds no place and no bytes in a scheduler: for a transport
  /// that delivers requests to the handler itself (the simulation harness's). Given once; dropped
  /// ungiven, it refuses (500 "abandoned"), as any Answer does.
  static Answer to(Reply reply);

 private:
  friend class RequestScheduler;
  struct State;
  explicit Answer(std::unique_ptr<State> state) noexcept;
  void abandon() noexcept;
  std::unique_ptr<State> _state;
};

/// Handles a request or notification on a worker and says what it did. May block; an exception is
/// a failure, answered 500.
using RequestHandler = std::function<Handled(const membership::NodeRecord& peer, sched::FrameType frame, const Message& request)>;
/// The same, owning the request (it may move the payload out, never copied) and given its Answer:
/// it may answer now by returning, or move the Answer out, return Handled::deferred() and answer
/// later. A lambda taking `const Message&` fits too.
using DeferringHandler = std::function<Handled(const membership::NodeRecord& peer, sched::FrameType frame, Message request, Answer& answer)>;

/// Queues peers' requests by class and runs them on bounded per-class workers (laws 1 to 4: no
/// class waits behind another's held workers).
///
/// A request that cannot be queued (its queue full, its class's or its peer's deferral bound
/// reached, or the ledger refusing its bytes) is answered at once with a 503 to try later, and
/// counted. All public members are thread-safe.
class RequestScheduler {
 public:
  /// @param routes each type's rules: its lane, whether it is ordered or fast control.
  /// @param limits the workers and bounds.
  /// @param handler runs each request.
  /// @param memory what a queued request's bytes are charged to; none: nothing is charged.
  /// @param time when a request was queued and ran, for the timings.
  /// @param threads where the workers are recorded; none: they are not.
  /// @param log where the workers log; none: nowhere.
  RequestScheduler(std::shared_ptr<const RouteTable> routes, ServerLimits limits, DeferringHandler handler, sched::RetainedMemoryLedger* memory = nullptr,
                   const contract::TimeSource* time = &contract::steady_time_source(), supervise::SupervisedThreads* threads = nullptr,
                   std::shared_ptr<status::LogSink> log = nullptr);
  /// The same, for a handler that always answers by returning.
  RequestScheduler(std::shared_ptr<const RouteTable> routes, ServerLimits limits, RequestHandler handler, sched::RetainedMemoryLedger* memory = nullptr,
                   const contract::TimeSource* time = &contract::steady_time_source(), supervise::SupervisedThreads* threads = nullptr,
                   std::shared_ptr<status::LogSink> log = nullptr);
  /// Stops.
  ~RequestScheduler();
  /// Not copyable.
  RequestScheduler(const RequestScheduler&) = delete;
  /// Not copyable.
  RequestScheduler& operator=(const RequestScheduler&) = delete;

  /// Queues `frame` from `peer`. Waits on nothing.
  /// @param reply how the answer goes back; empty for a notification.
  void submit(std::shared_ptr<const membership::NodeRecord> peer, Frame frame, Reply reply);
  /// The same with a record of its own, for a caller with no session's record to share.
  void submit(const membership::NodeRecord& peer, Frame frame, Reply reply) {
    submit(std::make_shared<const membership::NodeRecord>(peer), std::move(frame), std::move(reply));
  }
  /// The peer promoted a request still queued here to a more urgent frame type.
  void promote(const identity::NodeId& peer, uint64_t request_id, sched::FrameType frame);
  /// The peer cancelled a request still queued here: it is dropped unrun.
  void cancel(const identity::NodeId& peer, uint64_t request_id);
  /// Finishes the running jobs, drops the queues and joins the workers. Waits on the handlers in
  /// flight.
  void stop();

  /// The queues, counts and timings now.
  WorkStats stats() const;
  /// Whether stop() has begun: nothing queued after it runs.
  bool stopping() const;
  /// The route table it was given.
  const RouteTable& routes() const noexcept { return *_routes; }

  /// @cond internal
 private:
  enum class Queue { fast, control, ordered, foreground, read_ahead, loader, speculative };
  struct Job {
    // Its session's record, shared: a job never copies it (Dagda's profile).
    std::shared_ptr<const membership::NodeRecord> peer;
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
  const DeferringHandler _handler;
  sched::RetainedMemoryLedger* const _memory;

 public:
  // Requests running or deferred, by work class and by peer; each Answer gives its back.
  struct Answering {
    std::array<std::atomic<size_t>, 4> by_class{};
    contract::Mutex mutex;
    std::map<identity::NodeId, std::array<size_t, 4>> by_peer DANNAN_GUARDED_BY(mutex);
    void begin(const identity::NodeId& peer, size_t work);
    void end(const identity::NodeId& peer, size_t work);
    size_t of(const identity::NodeId& peer, size_t work);
  };

 private:
  // Taken inside _mutex at submit, never the other way round.
  const std::shared_ptr<Answering> _answering = std::make_shared<Answering>();
  const contract::TimeSource* const _time;
  const status::ScopedLogger _log;

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
  /// @endcond
};

}  // namespace dannan::net
