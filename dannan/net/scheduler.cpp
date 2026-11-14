//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/scheduler.h"

#include <algorithm>

#include "dannan/net/assembler.h"
#include "dannan/status/thread_cpu.h"

namespace dannan::net {
namespace {

Message overloaded() { return error_reply(503, "overloaded", "request queue full"); }

void record(Timing& timing, uint64_t queue_us, uint64_t handler_us) {
  ++timing.requests;
  timing.queue_wait_us_total += queue_us;
  timing.queue_wait_us_max = std::max(timing.queue_wait_us_max, queue_us);
  timing.handler_us_total += handler_us;
  timing.handler_us_max = std::max(timing.handler_us_max, handler_us);
}

uint64_t micros(contract::Clock::duration duration) {
  return static_cast<uint64_t>(std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(duration).count()));
}

}  // namespace

struct Answer::State {
  Reply reply;
  sched::RetainedMemoryLedger::Lease memory;
  std::shared_ptr<std::array<std::atomic<size_t>, 4>> answering;
  size_t work = 0;
  ~State() {
    if (answering) (*answering)[work].fetch_sub(1, std::memory_order_acq_rel);
  }
};

Answer::Answer() noexcept = default;
Answer::Answer(std::unique_ptr<State> state) noexcept : _state(std::move(state)) {}
Answer::Answer(Answer&& other) noexcept = default;
Answer& Answer::operator=(Answer&& other) noexcept {
  if (this != &other) {
    abandon();
    _state = std::move(other._state);
  }
  return *this;
}
Answer::~Answer() { abandon(); }

void Answer::operator()(Message reply) {
  // Taken first, so a later call finds nothing; the bound and the bytes are
  // given back once the reply is sent.
  auto state = std::move(_state);
  if (state && state->reply) state->reply(std::move(reply));
}

void Answer::fail(uint16_t status, std::string_view code, std::string_view why) { (*this)(error_reply(status, code, why)); }

void Answer::abandon() noexcept {
  if (!_state) return;
  try {
    (*this)(error_reply(500, "abandoned", "the request's handler dropped its answer"));
  } catch (...) {
    _state.reset();
  }
}

RequestScheduler::RequestScheduler(std::shared_ptr<const RouteTable> routes, ServerLimits limits, RequestHandler handler, sched::RetainedMemoryLedger* memory,
                                   const contract::TimeSource* time, supervise::SupervisedThreads* threads, std::shared_ptr<status::LogSink> log)
    : RequestScheduler(
          std::move(routes), limits,
          [handler = std::move(handler)](const membership::NodeRecord& peer, sched::FrameType frame, Message request, Answer&) {
            return handler(peer, frame, request);
          },
          memory, time, threads, std::move(log)) {}

RequestScheduler::RequestScheduler(std::shared_ptr<const RouteTable> routes, ServerLimits limits, DeferringHandler handler, sched::RetainedMemoryLedger* memory,
                                   const contract::TimeSource* time, supervise::SupervisedThreads* threads, std::shared_ptr<status::LogSink> log)
    : _routes(std::move(routes)),
      _limits(limits),
      _handler(std::move(handler)),
      _memory(memory),
      _time(time),
      _log(log ? std::move(log) : status::null_sink(), "net") {
  if (_limits.foreground_data_reserve >= _limits.data_workers) throw std::invalid_argument("the foreground reserve must leave DATA workers for other work");
  for (size_t i = 0; i < _limits.fast_control_workers; ++i)
    _workers.emplace_back(
        [this, threads](std::stop_token stop) { supervise::supervised_loop(threads, "net-rpc-fast-control-worker", stop, [this, stop] { fast_loop(stop); }); });
  for (size_t i = 0; i < _limits.control_workers; ++i)
    _workers.emplace_back(
        [this, threads](std::stop_token stop) { supervise::supervised_loop(threads, "net-rpc-control-worker", stop, [this, stop] { control_loop(stop); }); });
  for (size_t i = 0; i < _limits.ordered_workers; ++i)
    _workers.emplace_back(
        [this, threads](std::stop_token stop) { supervise::supervised_loop(threads, "net-rpc-metadata-worker", stop, [this, stop] { ordered_loop(stop); }); });
  for (size_t i = 0; i < _limits.data_workers; ++i)
    _workers.emplace_back(
        [this, threads](std::stop_token stop) { supervise::supervised_loop(threads, "net-rpc-data-worker", stop, [this, stop] { data_loop(stop); }); });
}

RequestScheduler::~RequestScheduler() { stop(); }

RequestScheduler::Queue RequestScheduler::queue_for(sched::FrameType frame) noexcept {
  switch (frame) {
    case sched::FrameType::control:
      return Queue::control;
    case sched::FrameType::foreground:
      return Queue::foreground;
    case sched::FrameType::read_ahead:
      return Queue::read_ahead;
    case sched::FrameType::loader:
      return Queue::loader;
    case sched::FrameType::speculative:
      return Queue::speculative;
  }
  return Queue::speculative;
}

std::deque<RequestScheduler::Job>& RequestScheduler::queue(Queue which) {
  switch (which) {
    case Queue::fast:
      return _fast;
    case Queue::control:
      return _control;
    case Queue::ordered:
      return _ordered;
    case Queue::foreground:
      return _foreground;
    case Queue::read_ahead:
      return _read_ahead;
    case Queue::loader:
      return _loader;
    case Queue::speculative:
      return _speculative;
  }
  return _speculative;
}

size_t* RequestScheduler::bytes_for(Queue which) {
  switch (which) {
    case Queue::fast:
      return &_fast_bytes;
    case Queue::control:
      return &_control_bytes;
    case Queue::ordered:
      return &_ordered_bytes;
    default:
      return &_data_bytes;
  }
}

void RequestScheduler::submit(membership::NodeRecord peer, Frame frame, Reply reply) {
  if (_routes->is_on_session(frame.message.type)) {
    {
      contract::Lock lock(_mutex);
      if (_stopping) return;
    }
    const auto now = _time->now();
    run(Job{std::move(peer), std::move(frame), std::move(reply), now, {}});
    return;
  }
  const auto bytes = frame.message.payload.size();
  bool refused = false;
  {
    contract::Lock lock(_mutex);
    if (_stopping) return;
    std::optional<sched::RetainedMemoryLedger::Lease> lease;
    if (_memory) {
      lease = _memory->try_acquire(sched::work_class(frame.frame), sched::MemoryOwner::rpc_frame, sizeof(Job) + (frame.message.memory ? 0 : bytes));
      refused = !lease;
    }
    Queue which;
    size_t byte_limit = 0;
    size_t job_limit = _limits.max_queued_jobs;
    if (_routes->is_ordered(frame.message.type)) {
      which = Queue::ordered;
      // A quarter of the queue is kept for control-class work: background
      // work cannot fill it and have a commit refused.
      const bool control = frame.frame == sched::FrameType::control;
      job_limit = control ? _limits.ordered_pending_jobs : _limits.ordered_pending_jobs - _limits.ordered_pending_jobs / 4;
      byte_limit = control ? _limits.ordered_pending_bytes : _limits.ordered_pending_bytes - _limits.ordered_pending_bytes / 4;
    } else if (frame.frame == sched::FrameType::control && _routes->is_fast_control(frame.message.type)) {
      which = Queue::fast;
      byte_limit = _limits.fast_control_pending_bytes;
    } else {
      which = queue_for(frame.frame);
      byte_limit = which == Queue::control ? _limits.control_pending_bytes : _limits.data_pending_bytes;
    }
    auto& jobs = queue(which);
    auto* held = bytes_for(which);
    if (!refused) refused = jobs.size() >= job_limit || bytes > byte_limit || *held > byte_limit - bytes;
    // A class's requests running or answered later are bounded too: a handler
    // that defers holds no worker, so the workers no longer bound them.
    const auto work = static_cast<size_t>(sched::work_class(frame.frame));
    if (!refused) refused = (*_answering)[work].load(std::memory_order_acquire) >= _limits.max_deferred;
    if (refused) {
      ++(which == Queue::ordered ? _ordered_rejected : _rejected);
    } else {
      *held += bytes;
      jobs.push_back(Job{std::move(peer), std::move(frame), std::move(reply), _time->now(), lease ? std::move(*lease) : sched::RetainedMemoryLedger::Lease{}});
    }
  }
  if (refused) {
    if (reply) reply(overloaded());
    return;
  }
  _changed.notify_all();
}

void RequestScheduler::promote(const identity::NodeId& peer, uint64_t request_id, sched::FrameType frame) {
  if (frame != sched::FrameType::foreground && frame != sched::FrameType::read_ahead) return;
  {
    contract::Lock lock(_mutex);
    for (const auto from : {Queue::speculative, Queue::loader, Queue::read_ahead, Queue::foreground}) {
      auto& jobs = queue(from);
      const auto found = std::find_if(jobs.begin(), jobs.end(), [&](const Job& job) { return job.peer.id == peer && job.frame.request_id == request_id; });
      if (found == jobs.end()) continue;
      found->frame.frame = more_urgent(frame, found->frame.frame);
      const auto to = queue_for(found->frame.frame);
      if (to != from) {
        auto job = std::move(*found);
        jobs.erase(found);
        queue(to).push_back(std::move(job));
      }
      break;
    }
  }
  _changed.notify_all();
}

void RequestScheduler::cancel(const identity::NodeId& peer, uint64_t request_id) {
  contract::Lock lock(_mutex);
  for (const auto which : {Queue::fast, Queue::control, Queue::ordered, Queue::foreground, Queue::read_ahead, Queue::loader, Queue::speculative}) {
    auto& jobs = queue(which);
    const auto found = std::find_if(jobs.begin(), jobs.end(), [&](const Job& job) { return job.peer.id == peer && job.frame.request_id == request_id; });
    if (found == jobs.end()) continue;
    *bytes_for(which) -= found->frame.message.payload.size();
    jobs.erase(found);
    return;
  }
}

void RequestScheduler::run(Job job) {
  if (job.frame.session_closed && job.frame.session_closed->load()) return;
  const auto started = _time->now();
  auto state = std::make_unique<Answer::State>();
  state->reply = std::move(job.reply);
  state->memory = std::move(job.memory);
  state->work = static_cast<size_t>(sched::work_class(job.frame.frame));
  (*_answering)[state->work].fetch_add(1, std::memory_order_acq_rel);
  state->answering = _answering;
  Answer answer(std::move(state));
  const auto type = job.frame.message.type;
  auto handled = Handled::failed(500, "internal_error", "request handler failed");
  try {
    handled = _handler(job.peer, job.frame.frame, std::move(job.frame.message), answer);
  } catch (const std::exception& failure) {
    handled = Handled::failed(500, "internal_error", failure.what());
  } catch (...) {
  }
  const auto finished = _time->now();
  const auto outcome = handled.outcome();
  if (outcome == Handled::Outcome::unrecognised)
    answer(error_reply(501, "not_implemented", "message " + _routes->name(type) + " is not handled here"));
  else if (outcome != Handled::Outcome::deferred)
    answer(handled.take_reply());
  // Deferred with the Answer still here: the handler kept nothing to give,
  // and dropping it refuses the call.
  answer = Answer();
  {
    contract::Lock lock(_timing_mutex);
    record(_frame_timings[job.frame.frame], micros(started - job.queued), micros(finished - started));
    auto& by_type = _message_timings[type];
    record(by_type, micros(started - job.queued), micros(finished - started));
    if (outcome == Handled::Outcome::failed) ++by_type.failed;
    if (outcome == Handled::Outcome::unrecognised) ++by_type.unrecognised;
    if (outcome == Handled::Outcome::deferred) {
      ++by_type.deferred;
      ++_frame_timings[job.frame.frame].deferred;
    }
  }
}

void RequestScheduler::fast_loop(std::stop_token) {
  status::ThreadCpuReporter cpu("net-rpc-fast-control-worker", std::chrono::seconds(5), _log);
  for (;;) {
    Job job;
    {
      contract::Lock lock(_mutex);
      _changed.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return _stopping || !_fast.empty(); });
      if (_stopping) return;
      job = std::move(_fast.front());
      _fast.pop_front();
      _fast_bytes -= job.frame.message.payload.size();
    }
    run(std::move(job));
    cpu.tick();
  }
}

void RequestScheduler::control_loop(std::stop_token) {
  status::ThreadCpuReporter cpu("net-rpc-control-worker", std::chrono::seconds(5), _log);
  for (;;) {
    Job job;
    {
      contract::Lock lock(_mutex);
      _changed.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return _stopping || !_control.empty(); });
      if (_stopping) return;
      job = std::move(_control.front());
      _control.pop_front();
      _control_bytes -= job.frame.message.payload.size();
    }
    run(std::move(job));
    cpu.tick();
  }
}

void RequestScheduler::ordered_loop(std::stop_token) {
  status::ThreadCpuReporter cpu("net-rpc-metadata-worker", std::chrono::seconds(5), _log);
  for (;;) {
    Job job;
    {
      contract::Lock lock(_mutex);
      const auto eligible = [&](const Job& candidate) DANNAN_REQUIRES(_mutex) { return !_ordered_active.contains(candidate.peer.id); };
      _changed.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) { return _stopping || std::any_of(_ordered.begin(), _ordered.end(), eligible); });
      if (_stopping) return;
      // The oldest control-class job first (a commit's accept), then the
      // oldest of the rest. Each flow awaits its own replies, so no
      // dependency is reordered.
      auto ready = std::find_if(_ordered.begin(), _ordered.end(), [&](const Job& candidate) DANNAN_REQUIRES(_mutex) {
        return eligible(candidate) && candidate.frame.frame == sched::FrameType::control;
      });
      if (ready == _ordered.end()) ready = std::find_if(_ordered.begin(), _ordered.end(), eligible);
      job = std::move(*ready);
      _ordered.erase(ready);
      _ordered_bytes -= job.frame.message.payload.size();
      _ordered_active.insert(job.peer.id);
    }
    const auto peer = job.peer.id;
    run(std::move(job));
    cpu.tick();
    {
      contract::Lock lock(_mutex);
      _ordered_active.erase(peer);
    }
    _changed.notify_all();
  }
}

bool RequestScheduler::data_ready_locked() const { return !_foreground.empty() || !_read_ahead.empty() || !_loader.empty() || !_speculative.empty(); }

void RequestScheduler::data_loop(std::stop_token) {
  status::ThreadCpuReporter cpu("net-rpc-data-worker", std::chrono::seconds(5), _log);
  const auto lower_limit = _limits.data_workers - _limits.foreground_data_reserve;
  for (;;) {
    Job job;
    bool nonforeground = false;
    {
      contract::Lock lock(_mutex);
      _changed.wait(lock.native(), [&]() DANNAN_REQUIRES(_mutex) {
        if (_stopping || !_foreground.empty()) return true;
        return _nonforeground_active < lower_limit && (!_read_ahead.empty() || !_loader.empty() || !_speculative.empty());
      });
      if (_stopping) return;
      Queue which;
      if (!_foreground.empty()) {
        which = Queue::foreground;
      } else {
        which = !_read_ahead.empty() ? Queue::read_ahead : !_loader.empty() ? Queue::loader : Queue::speculative;
        ++_nonforeground_active;
        nonforeground = true;
      }
      auto& jobs = queue(which);
      job = std::move(jobs.front());
      jobs.pop_front();
      _data_bytes -= job.frame.message.payload.size();
    }
    run(std::move(job));
    cpu.tick();
    if (nonforeground) {
      {
        contract::Lock lock(_mutex);
        --_nonforeground_active;
      }
      _changed.notify_all();
    }
  }
}

void RequestScheduler::stop() {
  size_t dropped = 0;
  {
    contract::Lock lock(_mutex);
    if (_stopping) return;
    _stopping = true;
    // Queued work is dropped unanswered: its sessions are closing.
    for (const auto which : {Queue::fast, Queue::control, Queue::ordered, Queue::foreground, Queue::read_ahead, Queue::loader, Queue::speculative}) {
      dropped += queue(which).size();
      queue(which).clear();
    }
    _fast_bytes = _control_bytes = _ordered_bytes = _data_bytes = 0;
  }
  _changed.notify_all();
  _log.debug("shutdown: RPC queued requests dropped count=" + std::to_string(dropped));
  _log.debug("shutdown: RPC workers joining");
  for (auto& worker : _workers) worker.join();
  _log.debug("shutdown: RPC workers joined");
}

bool RequestScheduler::stopping() const {
  contract::Lock lock(_mutex);
  return _stopping;
}

WorkStats RequestScheduler::stats() const {
  WorkStats out;
  {
    contract::Lock lock(_mutex);
    out.ordered_pending_jobs = _ordered.size();
    out.ordered_pending_bytes = _ordered_bytes;
    out.ordered_active_jobs = _ordered_active.size();
    out.ordered_rejected = _ordered_rejected;
    out.fast_control_pending_bytes = _fast_bytes;
    out.control_pending_bytes = _control_bytes;
    out.data_pending_bytes = _data_bytes;
    out.rejected = _rejected;
  }
  contract::Lock lock(_timing_mutex);
  out.frame_timings = _frame_timings;
  out.message_timings = _message_timings;
  return out;
}

}  // namespace dannan::net
