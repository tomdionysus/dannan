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
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace dannan::test {

// How a child is started, beyond its argv.
struct ChildOptions {
  // Its whole environment, "NAME=value" each; nothing: this process's.
  std::optional<std::vector<std::string>> environment;
  // Written to its standard input, which is then closed; nothing: its
  // standard input is /dev/null. Written on a thread of its own, so a child
  // that never reads cannot hold the test.
  std::optional<std::string> input;
  // Its standard output and error on a pseudo-terminal the test reads, so
  // it sees a terminal (the log is then unused).
  bool terminal = false;
};

// A process a test runs: its stdout piped to the test (or on a
// pseudo-terminal) and read a line or every byte at a time by deadline,
// its stderr to a log file. Ended with SIGTERM, then SIGKILL, when the
// object goes, unless it has already been waited for. Never waits without
// a deadline. For process-level tests: a node binary as a peer, a
// third-party client against a server, a command's refusals. Linking it
// needs libutil on Linux, for the pseudo-terminal.
class ChildProcess {
 public:
  // argv[0] is the binary's path. Throws std::runtime_error if it cannot be
  // started.
  ChildProcess(const std::vector<std::string>& argv, const std::filesystem::path& log, ChildOptions options = {}) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    int out[2] = {-1, -1};
    int in[2] = {-1, -1};
    const auto cleanup = [&] {
      for (const int fd : {out[0], out[1], in[0], in[1]})
        if (fd >= 0) ::close(fd);
      posix_spawn_file_actions_destroy(&actions);
    };
    if (options.terminal) {
      if (::openpty(&out[0], &out[1], nullptr, nullptr, nullptr) != 0) {
        cleanup();
        throw std::runtime_error("openpty failed");
      }
      posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
      posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
    } else {
      if (::pipe(out) != 0) {
        cleanup();
        throw std::runtime_error("pipe failed");
      }
      posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    posix_spawn_file_actions_addclose(&actions, out[0]);
    if (options.input) {
      // A socket, not a pipe: a write to a child that has gone is an error,
      // never a SIGPIPE in the test.
      if (::socketpair(AF_UNIX, SOCK_STREAM, 0, in) != 0) {
        cleanup();
        throw std::runtime_error("socketpair failed");
      }
#ifdef SO_NOSIGPIPE
      const int on = 1;
      ::setsockopt(in[1], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
      posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
      posix_spawn_file_actions_addclose(&actions, in[1]);
    } else {
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    std::vector<char*> args;
    for (const auto& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    std::vector<char*> env;
    if (options.environment) {
      for (const auto& entry : *options.environment) env.push_back(const_cast<char*>(entry.c_str()));
      env.push_back(nullptr);
    }
    const int spawned = posix_spawn(&_pid, args[0], &actions, nullptr, args.data(), options.environment ? env.data() : environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    if (in[0] >= 0) ::close(in[0]);
    _out = out[0];
    if (spawned != 0) {
      ::close(_out);
      if (in[1] >= 0) ::close(in[1]);
      _out = -1;
      _pid = -1;
      throw std::runtime_error("could not start " + argv[0]);
    }
    if (options.input) {
      _writer = std::thread([fd = in[1], input = std::move(*options.input)] {
        size_t done = 0;
        while (done < input.size()) {
#ifdef MSG_NOSIGNAL
          const auto n = ::send(fd, input.data() + done, input.size() - done, MSG_NOSIGNAL);
#else
          const auto n = ::send(fd, input.data() + done, input.size() - done, 0);
#endif
          if (n < 0 && errno == EINTR) continue;
          if (n <= 0) break;
          done += static_cast<size_t>(n);
        }
        ::shutdown(fd, SHUT_WR);
        ::close(fd);
      });
    }
  }
  ~ChildProcess() {
    if (_pid > 0 && !_status) {
      ::kill(_pid, SIGTERM);
      if (!wait(std::chrono::steady_clock::now() + std::chrono::seconds(10))) {
        ::kill(_pid, SIGKILL);
        ::waitpid(_pid, nullptr, 0);
      }
    }
    if (_out >= 0) ::close(_out);
    if (_writer.joinable()) _writer.join();
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  // The next line the child writes, without its line end; nothing by the
  // deadline, or at the end of its output.
  std::optional<std::string> line(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      if (const auto end = _buffer.find('\n'); end != std::string::npos) {
        auto out = _buffer.substr(0, end);
        if (!out.empty() && out.back() == '\r') out.pop_back();  // a terminal's line end
        _buffer.erase(0, end + 1);
        return out;
      }
      if (!fill(deadline)) return std::nullopt;
    }
  }

  // Every byte the child writes from here to the end of its output or the
  // deadline, raw: what it wrote is not split at line ends.
  std::string read_all(std::chrono::steady_clock::time_point deadline) {
    while (fill(deadline)) {
    }
    return std::exchange(_buffer, {});
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

  // Sends it `number` (SIGINT, SIGTERM, ...); wait() then gives how it
  // ended. Nothing once it has been waited for.
  void signal(int number) {
    if (_pid > 0 && !_status) ::kill(_pid, number);
  }
  void terminate() { signal(SIGTERM); }

  pid_t pid() const noexcept { return _pid; }

 private:
  // Reads what is there into the buffer; false at the end of output (a
  // closed pipe, or a terminal whose other side closed) or the deadline.
  bool fill(std::chrono::steady_clock::time_point deadline) {
    if (_out < 0) return false;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return false;
    pollfd ready{_out, POLLIN, 0};
    if (::poll(&ready, 1, static_cast<int>(left)) <= 0) return false;
    char chunk[4096];
    const auto got = ::read(_out, chunk, sizeof(chunk));
    if (got <= 0) return false;
    _buffer.append(chunk, static_cast<size_t>(got));
    return true;
  }

  pid_t _pid = -1;
  int _out = -1;
  std::string _buffer;
  std::optional<int> _status;
  std::thread _writer;
};

}  // namespace dannan::test
