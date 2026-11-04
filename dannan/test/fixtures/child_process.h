//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace dannan::test {

// A process a test runs: its stdout piped to the test and read a line at a
// time by deadline, its stderr to a log file. Ended with SIGTERM, then
// SIGKILL, when the object goes, unless it has already been waited for.
// Never waits without a deadline. For process-level tests: a node binary
// as a peer, a third-party client against a server.
class ChildProcess {
 public:
  // argv[0] is the binary's path. Throws std::runtime_error if it cannot be
  // started.
  ChildProcess(const std::vector<std::string>& argv, const std::filesystem::path& log) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) throw std::runtime_error("pipe failed");
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    const int spawned = posix_spawn(&_pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    _out = fds[0];
    if (spawned != 0) {
      ::close(_out);
      _out = -1;
      _pid = -1;
      throw std::runtime_error("could not start " + argv[0]);
    }
  }
  ~ChildProcess() {
    if (_out >= 0) ::close(_out);
    if (_pid <= 0 || _status) return;
    ::kill(_pid, SIGTERM);
    if (wait(std::chrono::steady_clock::now() + std::chrono::seconds(10))) return;
    ::kill(_pid, SIGKILL);
    ::waitpid(_pid, nullptr, 0);
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // The next line the child writes, without its line end; nothing by the
  // deadline, or at the end of its output.
  std::optional<std::string> line(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      if (const auto end = _buffer.find('\n'); end != std::string::npos) {
        auto out = _buffer.substr(0, end);
        _buffer.erase(0, end + 1);
        return out;
      }
      if (_out < 0) return std::nullopt;
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (left <= 0) return std::nullopt;
      pollfd ready{_out, POLLIN, 0};
      if (::poll(&ready, 1, static_cast<int>(left)) <= 0) return std::nullopt;
      char chunk[4096];
      const auto got = ::read(_out, chunk, sizeof(chunk));
      if (got <= 0) return std::nullopt;
      _buffer.append(chunk, static_cast<size_t>(got));
    }
  }

  // Waits for the child to exit, until the deadline: its exit code (0 to
  // 255), or 128 plus the signal that ended it; nothing if it is still
  // running at the deadline. Once it has exited, every call gives the same.
  std::optional<int> wait(std::chrono::steady_clock::time_point deadline) {
    while (!_status) {
      int status = 0;
      const auto reaped = ::waitpid(_pid, &status, WNOHANG);
      if (reaped == _pid) {
        _status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        break;
      }
      if (reaped < 0 || std::chrono::steady_clock::now() >= deadline) return _status;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return _status;
  }

  // Asks it to stop (SIGTERM); wait() then gives how it ended.
  void terminate() {
    if (_pid > 0 && !_status) ::kill(_pid, SIGTERM);
  }

  pid_t pid() const noexcept { return _pid; }

 private:
  pid_t _pid = -1;
  int _out = -1;
  std::string _buffer;
  std::optional<int> _status;
};

}  // namespace dannan::test
