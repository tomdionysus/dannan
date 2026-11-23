//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/contract/completion.h"
#include "dannan/contract/thread_safety.h"

/// @file
/// Running a program on this node: no shell, a fixed environment, each run its own process group.

namespace dannan::supervise {

/// A program to run (0.12.0; Louhi's net/command.h, moved here so Louhi and Macha link one copy).
///
/// The argument list is passed as it is, never through a shell, so no argument is ever parsed: the
/// caller checks every name it puts in one. The program's environment is PATH (the directories
/// below) and LC_ALL=C, nothing else.
struct Invocation {
  /// The program and its arguments. argv[0] is found on a fixed PATH of the system's own
  /// directories (/usr/sbin, /usr/bin, /sbin, /bin), never the caller's, unless it names a path.
  std::vector<std::string> argv;
  /// Written to its stdin, which is then closed.
  std::string input;
  /// Its stdout and stderr together go to this file instead of the result (created or emptied,
  /// mode 0600), which another reader may tail while it runs (Macha's mobitool). Unbounded.
  std::optional<std::filesystem::path> output_file;
  /// The most output kept in the result; past it the cut is said (CommandResult::truncated).
  size_t max_output = 64 * 1024;
  /// Its niceness, 0 to 19, set before it is given its input.
  int nice = 0;
};

/// How a run ended. On a timeout or a cancellation, what it had written so far, as evidence.
struct CommandResult {
  /// Its exit code, or 128 plus the signal that ended it.
  int exit_code = 0;
  /// Its stdout and stderr together, up to Invocation::max_output; empty with an output file.
  std::string output;
  /// Whether the output was cut at the bound.
  bool truncated = false;
};

/// A run's answer: the result, or why there is none.
using CommandOutcome = contract::Outcome<CommandResult>;

/// Runs programs on this node. An interface, so a hosted system's tests hand in a fake.
class Commands {
 public:
  virtual ~Commands() = default;
  /// Runs `invocation` until it exits.
  ///
  /// Waits on nothing; thread-safe.
  /// @param invocation the program, its input and where its output goes.
  /// @param deadline at it the run's whole process group is killed, and the run fails timed_out.
  /// @param executor where `done` is posted, once, never called inline.
  /// @param done the answer: success with the result (any exit code is a success), or unavailable
  ///   for a program that cannot be started (named), timed_out, or cancelled, each failure with the
  ///   output so far.
  /// @return an id for cancel().
  /// @throws std::invalid_argument for an empty argument list or a nice value outside 0 to 19.
  virtual uint64_t run(Invocation invocation, contract::Deadline deadline, contract::Executor executor, contract::Done<CommandOutcome> done) = 0;
  /// Kills the run's process group; it fails cancelled. Nothing for a run already answered.
  virtual void cancel(uint64_t id) = 0;
  /// Cancels every run, and every later run fails cancelled at once.
  virtual void stop() = 0;
};

/// Commands as child processes.
///
/// Each run is spawned (posix_spawn) as the leader of its own process group, so a deadline, a
/// cancel, a stop, or the leader's own exit kills everything it started: an `ip netns exec` child
/// never outlives its run (Louhi). Its pipes, deadline and reaping run on the io context, which
/// must outlive this; the leader is reaped by polling, never a blocking wait, and only after its
/// group is killed, so its id is never reused under the kill. No descriptor of this process
/// reaches the child but its stdin and output, and its signals are at their defaults.
///
/// @code
/// supervise::ProcessCommands commands(io);
/// supervise::Invocation ip;
/// ip.argv = {"ip", "link", "show", device};
/// commands.run(ip, std::chrono::seconds(5), executor, [](supervise::CommandOutcome out) {
///   if (!out.ok()) return;                       // unavailable, timed_out or cancelled
///   if (out.value.exit_code != 0) return;        // it ran, and said no
///   parse(out.value.output);
/// });
/// @endcode
class ProcessCommands final : public Commands {
 public:
  /// @param io where the runs' pipes, timers and reaping run; it must outlive this.
  explicit ProcessCommands(boost::asio::io_context& io) : _io(io) {}
  /// Stops: no child outlives its runner.
  ~ProcessCommands() override;

  /// As Commands::run().
  uint64_t run(Invocation invocation, contract::Deadline deadline, contract::Executor executor, contract::Done<CommandOutcome> done) override;
  /// As Commands::cancel().
  void cancel(uint64_t id) override;
  /// As Commands::stop().
  void stop() override;

  /// @cond internal
  class Run;
  // The runs in flight, shared with each run weakly, so one that ends after its runner is gone
  // finds nothing to tell.
  struct Registry {
    contract::Mutex mutex;
    std::map<uint64_t, std::weak_ptr<Run>> runs DANNAN_GUARDED_BY(mutex);
    uint64_t next DANNAN_GUARDED_BY(mutex) = 1;
    bool stopped DANNAN_GUARDED_BY(mutex) = false;
  };
  /// @endcond

 private:
  boost::asio::io_context& _io;
  const std::shared_ptr<Registry> _registry = std::make_shared<Registry>();
};

}  // namespace dannan::supervise
