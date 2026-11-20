//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

// The stack's callback vocabulary (TODO/2026-11-20-stack-plan.md, item 13, agreed by Lugh,
// Louhi, Dagda, Kvasir and Mimir). An asynchronous operation takes its arguments, then a
// deadline, the caller's executor and `done`. `done` is posted to that executor exactly once:
// with the answer, or with what the operation says when the deadline passes first. Never
// called inline, even when the answer is already known (the executor rule). The operation
// itself waits on nothing.
namespace dannan::contract {

using Executor = boost::asio::any_io_executor;

// How long an operation may take before its caller is answered without it.
using Deadline = std::chrono::milliseconds;

template <typename T>
using Done = std::function<void(T)>;

// Runs `task` once `after` has passed on the caller's injected time: Context::time().after,
// or a simulation's schedule. A task that fires after its Completion is gone does nothing.
using Schedule = std::function<void(std::chrono::milliseconds after, std::function<void()> task)>;

// Why an operation did not happen: one kind for every hosted system, so a caller branches on
// it the same way whichever system answered. The message says which side was at fault and
// what was wanted; the kind never carries that.
enum class Fault : uint8_t {
  none,
  // What was named does not exist, or was removed.
  not_found,
  // Not at the version the caller expected.
  conflict,
  // A name or key another record already has.
  duplicate,
  // Out of bounds or of the wrong shape: the caller's input, or a reply that could not be read.
  invalid,
  // Understood, and not allowed.
  refused,
  // Could not be reached, or failed inside before anything was changed.
  unavailable,
  // The deadline passed first. A write may still land.
  timed_out,
  // Failed, and nothing was changed that is not undone.
  failed,
};

const char* fault_name(Fault fault) noexcept;

// The value of an operation that returns nothing but that it happened.
struct Nothing {
  bool operator==(const Nothing&) const = default;
};

// A result with a reason a program can act on.
template <typename T = Nothing>
struct Outcome {
  Fault fault = Fault::none;
  std::string message;
  T value{};

  bool ok() const noexcept { return fault == Fault::none; }

  static Outcome success(T value = T{}) { return Outcome{Fault::none, {}, std::move(value)}; }
  static Outcome failure(Fault fault, std::string message) { return Outcome{fault, std::move(message), T{}}; }

  // The same failure, for an Outcome of another type.
  template <typename U>
  static Outcome from(const Outcome<U>& other) {
    return failure(other.fault, other.message);
  }
};

// The one answer an operation owes its caller. finish() posts the answer to the caller's
// executor; if the deadline passes first, the caller gets late() instead, and a finish() after
// that is dropped. Whichever comes first wins, once, from any thread. Held by shared_ptr: the
// deadline holds it weakly.
template <typename T>
class Completion : public std::enable_shared_from_this<Completion<T>> {
 public:
  // The deadline on a steady timer on `executor`, cancelled by the answer.
  static std::shared_ptr<Completion> start(Deadline deadline, Executor executor, Done<T> done, std::function<T()> late) {
    std::shared_ptr<Completion> completion(new Completion(std::move(executor), std::move(done), std::move(late)));
    completion->_timer = std::make_unique<boost::asio::steady_timer>(completion->_executor);
    completion->_timer->expires_after(deadline);
    completion->_timer->async_wait([weak = std::weak_ptr<Completion>(completion)](const boost::system::error_code& ec) {
      if (ec) return;
      if (const auto self = weak.lock()) self->answer_late();
    });
    return completion;
  }

  // The deadline on the caller's injected time (Context::time().after, a simulation's schedule).
  static std::shared_ptr<Completion> start(Deadline deadline, Executor executor, Done<T> done, std::function<T()> late, const Schedule& schedule) {
    std::shared_ptr<Completion> completion(new Completion(std::move(executor), std::move(done), std::move(late)));
    schedule(deadline, [weak = std::weak_ptr<Completion>(completion)] {
      if (const auto self = weak.lock()) self->answer_late();
    });
    return completion;
  }

  void finish(T value) { answer(std::move(value)); }

  // Whether the caller has had its answer, from finish() or the deadline: work not yet started
  // may be skipped.
  bool answered() const noexcept { return _answered.load(); }

 private:
  Completion(Executor executor, Done<T> done, std::function<T()> late) : _executor(std::move(executor)), _done(std::move(done)), _late(std::move(late)) {}

  void answer_late() {
    if (_answered.load()) return;
    answer(_late());
  }

  void answer(T value) {
    if (_answered.exchange(true)) return;
    boost::asio::post(_executor, [done = std::move(_done), value = std::move(value)]() mutable { done(std::move(value)); });
    // Cancelled on the executor, never on this thread, so a deadline and an answer on two
    // threads never touch the timer at once.
    if (_timer) boost::asio::post(_executor, [self = this->shared_from_this()] { self->_timer->cancel(); });
  }

  Executor _executor;
  std::unique_ptr<boost::asio::steady_timer> _timer;
  Done<T> _done;
  std::function<T()> _late;
  std::atomic<bool> _answered{false};
};

// Posts `value` to `done` on `executor`: an answer known at once still never arrives inline.
template <typename T>
void answer_now(const Executor& executor, Done<T> done, T value) {
  boost::asio::post(executor, [done = std::move(done), value = std::move(value)]() mutable { done(std::move(value)); });
}

}  // namespace dannan::contract
