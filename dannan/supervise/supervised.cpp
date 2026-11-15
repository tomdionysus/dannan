//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/supervise/supervised.h"

#include <condition_variable>
#include <mutex>

#include "dannan/status/process.h"

namespace dannan::supervise {

std::chrono::milliseconds SupervisedThreads::restart_delay(size_t consecutive, std::chrono::milliseconds initial, std::chrono::milliseconds ceiling) noexcept {
  auto delay = initial;
  for (size_t i = 1; i < consecutive && delay < ceiling; ++i) delay *= 2;
  return std::min(delay, ceiling);
}

void SupervisedThreads::adjust(std::string_view name, int running, int restarting) {
  contract::Lock lock(_mutex);
  auto found = _statuses.find(name);
  if (found == _statuses.end()) {
    SupervisedThreadStatus fresh;
    fresh.name = std::string(name);
    found = _statuses.emplace(fresh.name, std::move(fresh)).first;
  }
  found->second.running = static_cast<size_t>(static_cast<int64_t>(found->second.running) + running);
  found->second.restarting = static_cast<size_t>(static_cast<int64_t>(found->second.restarting) + restarting);
}

std::string SupervisedThreads::run(std::string_view name, const std::function<void()>& body) noexcept {
  std::string code;
  std::string fault;
  adjust(name, 1, 0);
  try {
    body();
  } catch (const std::exception& error) {
    code = "exception";
    fault = error.what();
  } catch (...) {
    code = "unknown_exception";
    fault = "unknown exception";
  }
  adjust(name, -1, 0);
  if (code.empty()) return {};
  contract::Lock lock(_mutex);
  auto& status = _statuses.at(std::string(name));
  ++status.faults;
  status.last_fault_code = code;
  status.last_fault = fault;
  status.last_fault_unix_ms = _wall->unix_ms();
  return fault.empty() ? code : fault;
}

void SupervisedThreads::say(std::string_view name, std::string_view outcome) noexcept {
  try {
    std::string code, fault;
    {
      contract::Lock lock(_mutex);
      const auto& status = _statuses.at(std::string(name));
      code = status.last_fault_code;
      fault = status.last_fault;
    }
    _log.error("thread '" + std::string(name) + "' faulted (" + code + "): " + fault + "; " + std::string(outcome));
  } catch (...) {
    // Saying so failed. Still nothing escapes a thread's entry point.
  }
}

void SupervisedThreads::loop(std::string_view name, std::stop_token stop, const std::function<void()>& body) noexcept {
  status::set_thread_name(name);
  size_t consecutive = 0;
  while (!stop.stop_requested()) {
    const auto started = _time->now();
    if (run(name, body).empty()) return;
    // A long healthy run earns a fresh backoff.
    consecutive = _time->now() - started > _ceiling ? 1 : consecutive + 1;
    if (stop.stop_requested()) {
      say(name, "stopping, not restarted");
      return;
    }
    say(name, "restarting in " + std::to_string(restart_delay(consecutive, _initial, _ceiling).count()) + " ms");
    adjust(name, 0, 1);
    {
      std::mutex mutex;
      std::unique_lock lock(mutex);
      std::condition_variable_any wake;
      wake.wait_for(lock, stop, restart_delay(consecutive, _initial, _ceiling), [] { return false; });
    }
    adjust(name, 0, -1);
  }
}

void supervised_loop(SupervisedThreads* threads, std::string_view name, std::stop_token stop, const std::function<void()>& body) noexcept {
  // Never destroyed: an owner held as a static (a process's log stage, say) may
  // be built before this and so outlive it, its thread still in loop() at exit.
  static SupervisedThreads* const unrecorded = new SupervisedThreads();
  (threads ? *threads : *unrecorded).loop(name, stop, body);
}

void SupervisedThreads::once(std::string_view name, const std::function<void()>& body) noexcept {
  if (!run(name, body).empty()) say(name, "thread ended");
}

void SupervisedThreads::escalating(std::string_view name, const std::function<void()>& body, const std::function<void(std::string)>& escalate) noexcept {
  auto fault = run(name, body);
  if (fault.empty()) return;
  say(name, "escalated to its subsystem's supervisor");
  if (!escalate) return;
  try {
    escalate("thread '" + std::string(name) + "' faulted: " + fault);
  } catch (...) {
  }
}

std::vector<SupervisedThreadStatus> SupervisedThreads::statuses() const {
  contract::Lock lock(_mutex);
  std::vector<SupervisedThreadStatus> out;
  for (const auto& [_, status] : _statuses) out.push_back(status);
  return out;
}

}  // namespace dannan::supervise
