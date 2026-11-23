//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/contract/time_wait.h"

#include <boost/asio/post.hpp>

namespace dannan::contract {

TimeWait::TimeWait(boost::asio::any_io_executor executor, const TimeSource& time, TimePoint at, std::function<void()> fn)
    : _executor(std::move(executor)), _time(time), _at(at), _fn(std::move(fn)) {}

std::shared_ptr<TimeWait> TimeWait::start(boost::asio::any_io_executor executor, const TimeSource& time, TimePoint at, std::function<void()> fn) {
  std::shared_ptr<TimeWait> wait(new TimeWait(std::move(executor), time, at, std::move(fn)));
  wait->begin();
  return wait;
}

TimeWait::~TimeWait() {
  uint64_t watch = 0;
  {
    Lock lock(_mutex);
    watch = _watch;
    _watch = 0;
  }
  if (watch) _time.unwatch(watch);
}

void TimeWait::begin() {
  // The watch holds the wait, as a timer's handler would, until it fires or is cancelled: both
  // unwatch, which lets it go.
  const auto id = _time.watch([self = shared_from_this(), &time = _time, at = _at] {
    // On the mover's thread: only a move that reaches `at` posts anything.
    if (time.now() >= at) self->fire();
  });
  if (id) {
    // A fire or cancel on another thread before the id was kept could not unwatch it: given up here.
    bool over = false;
    {
      Lock lock(_mutex);
      over = _over.load();
      if (!over) _watch = id;
    }
    if (over) return _time.unwatch(id);
    // A move before the watch began is seen here.
    if (_time.now() >= _at) fire();
    return;
  }
  boost::asio::post(_executor, [self = shared_from_this()] { self->poll(); });
}

void TimeWait::poll() {
  if (_over.load()) return;
  if (_time.now() >= _at) return fire();
  if (!_timer) _timer = std::make_unique<boost::asio::steady_timer>(_executor);
  _timer->expires_after(_time.wake_within(std::max(std::chrono::ceil<std::chrono::milliseconds>(_at - _time.now()), std::chrono::milliseconds(0))));
  _timer->async_wait([self = shared_from_this()](const boost::system::error_code& error) {
    if (!error) self->poll();
  });
}

void TimeWait::fire() {
  if (_over.exchange(true)) return;
  std::function<void()> fn;
  uint64_t watch = 0;
  {
    Lock lock(_mutex);
    fn = std::move(_fn);
    watch = _watch;
    _watch = 0;
  }
  if (watch) _time.unwatch(watch);
  boost::asio::post(_executor, std::move(fn));
}

void TimeWait::cancel() {
  if (_over.exchange(true)) return;
  uint64_t watch = 0;
  {
    Lock lock(_mutex);
    _fn = nullptr;
    watch = _watch;
    _watch = 0;
  }
  if (watch) _time.unwatch(watch);
  // The timer is the executor's: cancelled there.
  boost::asio::post(_executor, [self = shared_from_this()] {
    if (self->_timer) self->_timer->cancel();
  });
}

}  // namespace dannan::contract
