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

// Running a program on this node (TODO/2026-11-20-stack-plan.md, item 23; Louhi's net/command.h,
// moved here so Louhi and Macha link one copy). The argument list is passed as it is, never
// through a shell, so no argument is ever parsed: the caller checks every name it puts in one.
namespace dannan::supervise {

// A program and its arguments, argv[0] found on a fixed PATH of the system's own directories
// (/usr/sbin, /usr/bin, /sbin, /bin) unless it names a path; what is written to its stdin; and
// where its output goes. Its environment is PATH (those directories) and LC_ALL=C, nothing else.
struct Invocation {
  std::vector<std::string> argv;
  std::string input;
  // Its stdout and stderr together: kept in the result up to `max_output`, or written to this
  // file instead (created or emptied, mode 0600), which another reader may tail while it runs.
  std::optional<std::filesystem::path> output_file;
  size_t max_output = 64 * 1024;
  // Its niceness, 0 to 19, set before it is given its input.
  int nice = 0;
};

// How it ended: its exit code (128 + the signal for one killed), and its output, cut at the
// bound with `truncated` said. On a timeout or a cancellation, the output so far, as evidence.
struct CommandResult {
  int exit_code = 0;
  std::string output;
  bool truncated = false;
};

using CommandOutcome = contract::Outcome<CommandResult>;

class Commands {
 public:
  virtual ~Commands() = default;
  // Runs `invocation` until it exits. At `deadline` its whole process group is killed and the
  // call fails timed_out; a program that cannot be started fails unavailable, naming it. `done`
  // is posted to `executor` once, never called inline. Returns an id for cancel(). Throws
  // std::invalid_argument for an empty argument list or a nice value outside 0 to 19. Waits on
  // nothing; thread-safe.
  virtual uint64_t run(Invocation invocation, contract::Deadline deadline, contract::Executor executor, contract::Done<CommandOutcome> done) = 0;
  // Kills the run's process group; it fails cancelled. Nothing for a run already answered.
  virtual void cancel(uint64_t id) = 0;
  // Cancels every run, and every later one fails cancelled at once.
  virtual void stop() = 0;
};

// Commands as child processes (posix_spawn, each its own process group), their pipes, deadline
// and reaping on the io context, which outlives this. Reaped by polling, never a blocking wait.
// No descriptor of this process reaches the child but its stdin and output.
class ProcessCommands final : public Commands {
 public:
  explicit ProcessCommands(boost::asio::io_context& io) : _io(io) {}
  // Stops: no child outlives its runner.
  ~ProcessCommands() override;

  uint64_t run(Invocation invocation, contract::Deadline deadline, contract::Executor executor, contract::Done<CommandOutcome> done) override;
  void cancel(uint64_t id) override;
  void stop() override;

  class Run;
  // The runs in flight, shared with each run weakly, so one that ends after its runner is gone
  // finds nothing to tell.
  struct Registry {
    contract::Mutex mutex;
    std::map<uint64_t, std::weak_ptr<Run>> runs DANNAN_GUARDED_BY(mutex);
    uint64_t next DANNAN_GUARDED_BY(mutex) = 1;
    bool stopped DANNAN_GUARDED_BY(mutex) = false;
  };

 private:
  boost::asio::io_context& _io;
  const std::shared_ptr<Registry> _registry = std::make_shared<Registry>();
};

}  // namespace dannan::supervise
