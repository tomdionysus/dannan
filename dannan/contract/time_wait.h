//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// A wait on injected time that arms no timer while a stepped clock stands still.

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"

namespace dannan::contract {

/// Runs a task once a TimeSource's time reaches a point (0.12.0).
///
/// The task is posted to the executor given, never run inline, and not at all if the wait is
/// cancelled first. On a source that says when it moves (TimeSource::watch, as test::SteppedTime
/// does) no timer is armed: a move that reaches the point fires the wait, and a move that does not
/// posts nothing, so a frozen test clock with thousands of waits costs nothing. Any other source
/// keeps one steady timer that looks again every TimeSource::wake_within; the real clock's answer
/// is exact, so that is one wake.
///
/// Why: thousands of waiters polling a frozen test clock every 5 ms starved a shared io_context
/// under TSan (Dagda, 2026-11-20).
///
/// Ownership: the wait keeps itself until it fires or is cancelled, as a pending timer does, so a
/// caller may drop the pointer start() returns. The TimeSource must outlive it.
///
/// Thread safety: every member may be called from any thread; nothing waits.
///
/// @code
/// auto wait = contract::TimeWait::start(executor, time, time.now() + std::chrono::seconds(5),
///                                       [] { /* five seconds of `time` have passed */ });
/// // ... the answer came first:
/// wait->cancel();
/// @endcode
class TimeWait : public std::enable_shared_from_this<TimeWait> {
 public:
  /// Begins a wait.
  /// @param executor where `fn` is posted when the time is reached.
  /// @param time the source the point is in; it must outlive the wait.
  /// @param at the point: a time already reached fires at once, still posted.
  /// @param fn the task, run at most once.
  /// @return the wait, for cancel(); it need not be kept.
  static std::shared_ptr<TimeWait> start(boost::asio::any_io_executor executor, const TimeSource& time, TimePoint at, std::function<void()> fn);
  /// Stops watching the source, if it still is.
  ~TimeWait();
  /// Not copyable: one wait, one task.
  TimeWait(const TimeWait&) = delete;
  /// Not copyable.
  TimeWait& operator=(const TimeWait&) = delete;

  /// Cancels the wait: its task is not run if it has not begun. Cancelling one that has fired, or
  /// cancelling twice, does nothing. Waits on nothing.
  void cancel();
  /// Whether the wait has fired or been cancelled.
  bool over() const noexcept { return _over.load(); }

 private:
  TimeWait(boost::asio::any_io_executor executor, const TimeSource& time, TimePoint at, std::function<void()> fn);
  void begin();
  void fire();
  void poll();

  const boost::asio::any_io_executor _executor;
  const TimeSource& _time;
  const TimePoint _at;
  std::atomic<bool> _over{false};
  Mutex _mutex;
  std::function<void()> _fn DANNAN_GUARDED_BY(_mutex);
  uint64_t _watch DANNAN_GUARDED_BY(_mutex) = 0;
  // Only for a source that does not say when it moves; touched only on the executor.
  std::unique_ptr<boost::asio::steady_timer> _timer;
};

}  // namespace dannan::contract
