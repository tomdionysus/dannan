//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Deadlines and Outcome handling (docs/guide/faults.md): an operation of the stack's shape (its
// arguments, a deadline, the caller's executor, done) written with Completion, answered in time,
// and answered by its deadline when the work is too slow; and a program run past its deadline,
// whose failure carries the output so far as evidence. Each answer is branched on its Fault.
// Exits 0 when every answer was the one expected.
//
//   deadlines

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "dannan/contract/completion.h"
#include "dannan/supervise/command.h"

using namespace std::chrono_literals;
using dannan::contract::Fault;
using Answer = dannan::contract::Outcome<std::string>;

namespace {

// An operation of the stack's shape: its work takes `takes`; its caller is answered within
// `deadline` either way, once, never inline.
void slow_lookup(boost::asio::io_context& io, std::chrono::milliseconds takes, dannan::contract::Deadline deadline, dannan::contract::Executor executor,
                 dannan::contract::Done<Answer> done) {
  auto completion = dannan::contract::Completion<Answer>::start(deadline, executor, std::move(done), [] {
    // What the caller is told if the deadline passes first.
    return Answer::failure(Fault::timed_out, "the lookup did not finish within its deadline");
  });
  auto work = std::make_shared<boost::asio::steady_timer>(io, takes);
  work->async_wait([completion, work](const boost::system::error_code&) {
    // Dropped if the deadline already answered.
    completion->finish(Answer::success("the value"));
  });
}

// What a caller does with an answer: branch on its fault, never on a boolean.
std::string handle(const Answer& answer) {
  switch (answer.fault) {
    case Fault::none:
      return "got " + answer.value;
    case Fault::timed_out:
      return "timed out: " + answer.message;
    default:
      return std::string(dannan::contract::fault_name(answer.fault)) + ": " + answer.message;
  }
}

}  // namespace

int main() {
  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  std::thread runner([&io] { io.run(); });

  const auto ask = [&](std::chrono::milliseconds takes, std::chrono::milliseconds deadline) {
    std::promise<Answer> answered;
    slow_lookup(io, takes, deadline, io.get_executor(), [&](Answer answer) { answered.set_value(std::move(answer)); });
    return answered.get_future().get();
  };
  const auto quick = ask(10ms, 1s);
  std::printf("quick: %s\n", handle(quick).c_str());
  const auto slow = ask(2s, 100ms);
  std::printf("slow: %s\n", handle(slow).c_str());

  // A program run past its deadline: its whole process group killed, the run failed timed_out, and
  // what it had written so far kept as evidence.
  dannan::supervise::ProcessCommands commands(io);
  dannan::supervise::Invocation invocation;
  invocation.argv = {"sh", "-c", "echo started; sleep 30"};
  std::promise<dannan::supervise::CommandOutcome> ran;
  commands.run(invocation, 300ms, io.get_executor(), [&](dannan::supervise::CommandOutcome out) { ran.set_value(std::move(out)); });
  const auto run = ran.get_future().get();
  std::printf("program: %s, evidence: %s", dannan::contract::fault_name(run.fault), run.value.output.c_str());

  work.reset();
  runner.join();
  const bool expected = quick.ok() && slow.fault == Fault::timed_out && run.fault == Fault::timed_out && run.value.output == "started\n";
  return expected ? 0 : 1;
}
