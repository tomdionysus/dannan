//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/supervise/command.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace dannan::supervise {

namespace asio = boost::asio;

namespace {

using Socket = asio::local::stream_protocol::socket;

// Where a program is looked for: the system's own directories, never the caller's PATH.
constexpr std::array<const char*, 4> search_path{"/usr/sbin", "/usr/bin", "/sbin", "/bin"};
constexpr std::chrono::milliseconds reap_every{10};

std::string resolve(const std::string& program) {
  if (program.find('/') != std::string::npos) return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
  for (const char* directory : search_path) {
    const std::string candidate = std::string(directory) + "/" + program;
    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
  }
  return {};
}

}  // namespace

// One run: its child (the leader of its own process group), its sockets and its timers, on one
// strand. Every member is the strand's.
class ProcessCommands::Run : public std::enable_shared_from_this<Run> {
 public:
  Run(asio::io_context& io, uint64_t id, std::weak_ptr<Registry> registry, contract::Executor executor, contract::Done<CommandOutcome> done)
      : _strand(asio::make_strand(io)),
        _input(_strand),
        _output(_strand),
        _deadline(_strand),
        _reap(_strand),
        _id(id),
        _registry(std::move(registry)),
        _executor(std::move(executor)),
        _done(std::move(done)) {}

  ~Run() {
    if (_file >= 0) ::close(_file);
  }

  void start(Invocation invocation, contract::Deadline deadline) {
    asio::dispatch(_strand,
                   [self = shared_from_this(), invocation = std::move(invocation), deadline]() mutable { self->begin(std::move(invocation), deadline); });
  }

  // Kills the group; the run fails cancelled once its leader is reaped.
  void cancel() {
    asio::dispatch(_strand, [self = shared_from_this()] {
      if (!self->_running) return;
      self->_cancelled = true;
      self->kill_group();
    });
  }

 private:
  void begin(Invocation invocation, contract::Deadline deadline) {
    const auto& program = invocation.argv.front();
    _max_output = invocation.max_output;
    const std::string path = resolve(program);
    if (path.empty()) return answer(CommandOutcome::failure(contract::Fault::unavailable, "starting " + program + ": no such program"));

    Socket child_input(_strand), child_output(_strand);
    boost::system::error_code failed;
    asio::local::connect_pair(_input, child_input, failed);
    if (!failed && !invocation.output_file) asio::local::connect_pair(_output, child_output, failed);
    if (failed) return answer(CommandOutcome::failure(contract::Fault::unavailable, "starting " + program + ": " + failed.message()));
    int output_fd = invocation.output_file ? -1 : child_output.native_handle();
    if (invocation.output_file) {
      _file = ::open(invocation.output_file->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
      if (_file < 0)
        return answer(CommandOutcome::failure(contract::Fault::unavailable, "opening " + invocation.output_file->string() + ": " + std::strerror(errno)));
      output_fd = _file;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, child_input.native_handle(), STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output_fd, STDERR_FILENO);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // Nothing of this process reaches the child but its stdin and output.
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
#endif
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Its own process group, so a deadline, a cancel or its own exit kills whatever it started;
    // signals at their defaults and none blocked, whatever this process ignores or blocks.
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGHUP);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<char*> argv;
    for (auto& argument : invocation.argv) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::string path_variable = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
    std::string locale = "LC_ALL=C";
    std::array<char*, 3> environment{path_variable.data(), locale.data(), nullptr};
    const int spawned = ::posix_spawn(&_pid, path.c_str(), &actions, &attributes, argv.data(), environment.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    // The child holds its ends now; the parent's copies would keep its output open past its exit.
    child_input.close();
    child_output.close();
    if (_file >= 0) {
      ::close(_file);
      _file = -1;
    }
    if (spawned != 0) return answer(CommandOutcome::failure(contract::Fault::unavailable, "starting " + program + ": " + std::strerror(spawned)));
    _running = true;
    // Before it is given its input, so whatever it reads it reads at its niceness.
    if (invocation.nice > 0) (void)::setpriority(PRIO_PROCESS, static_cast<id_t>(_pid), invocation.nice);

    _deadline.expires_after(deadline);
    _deadline.async_wait([self = shared_from_this()](const boost::system::error_code& ec) {
      if (ec || !self->_running) return;
      self->_timed_out = true;
      self->kill_group();
    });
    write(std::move(invocation.input));
    if (invocation.output_file)
      _output_closed = true;
    else
      read();
    reap();
  }

  void kill_group() {
    // The leader is never reaped before its group is killed, so its id names this group still.
    if (_pid > 0 && !_reaped) (void)::kill(-_pid, SIGKILL);
  }

  void write(std::string input) {
    if (input.empty()) return close_input();
    auto held = std::make_shared<std::string>(std::move(input));
    // A child that exits without reading ends the write with an error, never a signal: sockets
    // are written without SIGPIPE.
    asio::async_write(_input, asio::buffer(*held), [self = shared_from_this(), held](const boost::system::error_code&, size_t) { self->close_input(); });
  }

  void close_input() {
    boost::system::error_code ignored;
    _input.shutdown(Socket::shutdown_send, ignored);
    _input.close(ignored);
  }

  void read() {
    _output.async_read_some(asio::buffer(_chunk), [self = shared_from_this()](const boost::system::error_code& ec, size_t got) {
      if (got > 0) {
        const size_t room = self->_max_output - std::min(self->_max_output, self->_result.output.size());
        self->_result.output.append(self->_chunk.data(), std::min(room, got));
        if (got > room) self->_result.truncated = true;
      }
      if (ec) {
        boost::system::error_code ignored;
        self->_output.close(ignored);
        self->_output_closed = true;
        return self->maybe_finish();
      }
      self->read();
    });
  }

  // Polls for the leader's exit without blocking the io context. On it, whatever the leader left
  // in its group is killed, then the leader is reaped.
  void reap() {
    siginfo_t info{};
    if (::waitid(P_PID, static_cast<id_t>(_pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0) {
      _reap.expires_after(reap_every);
      _reap.async_wait([self = shared_from_this()](const boost::system::error_code&) { self->reap(); });
      return;
    }
    kill_group();
    int status = 0;
    const pid_t gone = ::waitpid(_pid, &status, 0);
    _reaped = true;
    _running = false;
    _deadline.cancel();
    if (gone < 0) return answer(CommandOutcome::failure(contract::Fault::unavailable, std::string("waiting for the program: ") + std::strerror(errno)));
    _result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    maybe_finish();
  }

  // Answered once the leader is reaped and its output has ended (the group's killed, so it does).
  void maybe_finish() {
    if (!_reaped || !_output_closed) return;
    if (_timed_out)
      return answer(CommandOutcome::failure(contract::Fault::timed_out, "the program did not end within its deadline, and its group was killed", _result));
    if (_cancelled) return answer(CommandOutcome::failure(contract::Fault::cancelled, "the run was cancelled, and its group was killed", _result));
    answer(CommandOutcome::success(_result));
  }

  void answer(CommandOutcome outcome) {
    if (!_done) return;
    contract::answer_now(_executor, std::move(_done), std::move(outcome));
    _done = nullptr;
    if (const auto registry = _registry.lock()) {
      contract::Lock lock(registry->mutex);
      registry->runs.erase(_id);
    }
  }

  asio::strand<asio::io_context::executor_type> _strand;
  Socket _input;
  Socket _output;
  asio::steady_timer _deadline;
  asio::steady_timer _reap;
  const uint64_t _id;
  const std::weak_ptr<Registry> _registry;
  contract::Executor _executor;
  contract::Done<CommandOutcome> _done;
  std::array<char, 4096> _chunk{};
  CommandResult _result;
  size_t _max_output = 0;
  int _file = -1;
  pid_t _pid = 0;
  bool _running = false;
  bool _reaped = false;
  bool _output_closed = false;
  bool _timed_out = false;
  bool _cancelled = false;
};

ProcessCommands::~ProcessCommands() { stop(); }

uint64_t ProcessCommands::run(Invocation invocation, contract::Deadline deadline, contract::Executor executor, contract::Done<CommandOutcome> done) {
  if (invocation.argv.empty() || invocation.argv.front().empty()) throw std::invalid_argument("a command needs a program to run");
  if (invocation.nice < 0 || invocation.nice > 19) throw std::invalid_argument("a command's nice value is 0 to 19");
  std::shared_ptr<Run> run;
  uint64_t id = 0;
  {
    contract::Lock lock(_registry->mutex);
    id = _registry->next++;
    if (!_registry->stopped) {
      run = std::make_shared<Run>(_io, id, _registry, executor, std::move(done));
      _registry->runs.emplace(id, run);
    }
  }
  if (!run) {
    contract::answer_now(executor, std::move(done), CommandOutcome::failure(contract::Fault::cancelled, "the command runner has stopped"));
    return id;
  }
  // Begun on its strand: a spawn waits on nothing, and every completion is posted.
  run->start(std::move(invocation), deadline);
  return id;
}

void ProcessCommands::cancel(uint64_t id) {
  std::shared_ptr<Run> run;
  {
    contract::Lock lock(_registry->mutex);
    if (const auto found = _registry->runs.find(id); found != _registry->runs.end()) run = found->second.lock();
  }
  if (run) run->cancel();
}

void ProcessCommands::stop() {
  std::vector<std::shared_ptr<Run>> runs;
  {
    contract::Lock lock(_registry->mutex);
    _registry->stopped = true;
    for (const auto& [_, weak] : _registry->runs)
      if (auto run = weak.lock()) runs.push_back(std::move(run));
  }
  for (const auto& run : runs) run->cancel();
}

}  // namespace dannan::supervise
