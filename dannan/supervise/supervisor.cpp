//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/supervise/supervisor.h"

#include <dlfcn.h>

#include <condition_variable>
#include <thread>

#include "dannan/contract/thread_safety.h"

namespace dannan::supervise {

const char* state_name(SubsystemState state) noexcept {
  switch (state) {
    case SubsystemState::unavailable:
      return "unavailable";
    case SubsystemState::starting:
      return "starting";
    case SubsystemState::running:
      return "running";
    case SubsystemState::faulted:
      return "faulted";
    case SubsystemState::restarting:
      return "restarting";
    case SubsystemState::disabled:
      return "disabled";
  }
  return "unknown";
}

struct Supervisor::Entry {
  std::string name;
  SubsystemFactory factory;
  // Libraries are never closed: core may hold their code (a deleter, a
  // vtable) past the instance's life.
  void* handle = nullptr;

  mutable contract::Mutex mutex;
  std::condition_variable_any changed;
  SubsystemState state DANNAN_GUARDED_BY(mutex) = SubsystemState::starting;
  size_t restart_count DANNAN_GUARDED_BY(mutex) = 0;
  std::string last_fault DANNAN_GUARDED_BY(mutex);
  std::unique_ptr<Subsystem> instance DANNAN_GUARDED_BY(mutex);
  bool fault_requested DANNAN_GUARDED_BY(mutex) = false;
  bool stopping DANNAN_GUARDED_BY(mutex) = false;
  std::string pending_fault DANNAN_GUARDED_BY(mutex);
  std::jthread lifecycle;
};

Supervisor::Supervisor(SupervisorOptions options) : _options(std::move(options)) {}

Supervisor::~Supervisor() { stop(); }

void Supervisor::add_builtin(std::string name, SubsystemFactory factory) {
  if (!factory) return;
  auto entry = std::make_unique<Entry>();
  entry->name = std::move(name);
  entry->factory = std::move(factory);
  _entries.push_back(std::move(entry));
}

void Supervisor::start(SubsystemContext context) {
  _context = std::move(context);
  discover();
  for (auto& entry : _entries) {
    {
      contract::Lock lock(entry->mutex);
      if (entry->state == SubsystemState::disabled) continue;  // refused at load
    }
    auto* raw = entry.get();
    raw->lifecycle = std::jthread(
        [this, raw](std::stop_token stop) { supervised_loop(_options.threads, "subsystem-lifecycle", stop, [this, raw, stop] { run(*raw, stop); }); });
  }
}

void Supervisor::discover() {
  std::error_code error;
  if (_options.plugin_dir.empty() || !std::filesystem::is_directory(_options.plugin_dir, error)) return;
  for (const auto& candidate : std::filesystem::directory_iterator(_options.plugin_dir, error)) {
    const auto extension = candidate.path().extension();
    if (!candidate.is_regular_file() || (extension != ".so" && extension != ".dylib")) continue;
    auto entry = std::make_unique<Entry>();
    entry->name = candidate.path().stem().string();
    const auto refuse = [&](std::string why) {
      contract::Lock lock(entry->mutex);
      entry->state = SubsystemState::disabled;
      entry->last_fault = std::move(why);
    };
    entry->handle = ::dlopen(candidate.path().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!entry->handle) {
      refuse(std::string("failed to load: ") + ::dlerror());
      _entries.push_back(std::move(entry));
      continue;
    }
    ::dlerror();
    auto* symbol = ::dlsym(entry->handle, _options.entry_symbol.c_str());
    if (::dlerror() || !symbol) {
      // Not a plugin of this system: a directory may hold other libraries.
      // None of its code was called, so it may be unloaded.
      ::dlclose(entry->handle);
      continue;
    }
    const auto* plugin = reinterpret_cast<PluginEntryFunction>(symbol)();
    if (!plugin) {
      refuse("its entry function returned null");
    } else if (plugin->build_identity != _options.build_identity) {
      refuse("build identity mismatch: plugin " + std::string(plugin->build_identity) + ", core " + _options.build_identity + "; refused (a partial deploy?)");
    } else {
      entry->factory = [plugin](const SubsystemContext& context) { return plugin->create(context); };
    }
    _entries.push_back(std::move(entry));
  }
}

void Supervisor::run(Entry& entry, std::stop_token stop) {
  sched::RetryState retry;
  while (!stop.stop_requested()) {
    {
      contract::Lock lock(entry.mutex);
      entry.state = entry.restart_count ? SubsystemState::restarting : SubsystemState::starting;
      entry.fault_requested = false;
      entry.pending_fault.clear();
    }
    std::unique_ptr<Subsystem> instance;
    bool declined = false;
    std::string fault;
    try {
      auto attempt = _context;
      attempt.startup_stop = stop;
      instance = entry.factory(attempt);
      if (instance) {
        // Before start(): threads started inside it may report.
        instance->attach_fault_sink([&entry](std::string reason) {
          contract::Lock lock(entry.mutex);
          if (entry.fault_requested || entry.stopping) return;  // first reason wins
          entry.fault_requested = true;
          entry.pending_fault = reason.empty() ? std::string("subsystem reported a fault") : std::move(reason);
          entry.changed.notify_all();
        });
        instance->start();
      } else {
        declined = true;
      }
    } catch (const std::exception& error) {
      fault = error.what();
    } catch (...) {
      fault = "unknown exception";
    }

    if (declined) {
      contract::Lock lock(entry.mutex);
      entry.state = SubsystemState::unavailable;
      return;
    }
    if (fault.empty()) {
      {
        contract::Lock lock(entry.mutex);
        entry.state = SubsystemState::running;
        entry.instance = std::move(instance);
      }
      // Resets the backoff and keeps the window, so a flapping subsystem
      // still ends disabled.
      retry.succeeded();
      bool faulted;
      std::unique_ptr<Subsystem> owned;
      {
        contract::Lock lock(entry.mutex);
        entry.changed.wait(lock.native(), stop, [&entry]() DANNAN_REQUIRES(entry.mutex) { return entry.fault_requested; });
        faulted = entry.fault_requested;
        if (faulted) fault = std::move(entry.pending_fault);
        if (!faulted) entry.stopping = true;
        entry.fault_requested = false;
        owned = std::move(entry.instance);
      }
      if (owned) {
        try {
          owned->stop();
        } catch (...) {
        }
      }
      // Destroyed before a replacement is built: its sink points here.
      owned.reset();
      if (!faulted) return;
    } else {
      instance.reset();
    }
    {
      contract::Lock lock(entry.mutex);
      entry.last_fault = fault;
      ++entry.restart_count;
    }
    const auto delay = retry.failed(_options.policy, _options.time->now());
    contract::Lock lock(entry.mutex);
    if (!delay) {
      entry.state = SubsystemState::disabled;
      return;
    }
    entry.state = SubsystemState::faulted;
    entry.changed.wait_for(lock.native(), stop, *delay, [] { return false; });
  }
}

void Supervisor::stop() {
  for (auto& entry : _entries) {
    if (!entry->lifecycle.joinable()) continue;
    entry->lifecycle.request_stop();
    entry->changed.notify_all();
  }
  for (auto& entry : _entries)
    if (entry->lifecycle.joinable()) entry->lifecycle.join();
}

std::vector<SubsystemStatus> Supervisor::statuses() const {
  std::vector<SubsystemStatus> out;
  for (const auto& entry : _entries) {
    contract::Lock lock(entry->mutex);
    out.push_back({entry->name, entry->state, entry->restart_count, entry->last_fault});
  }
  return out;
}

}  // namespace dannan::supervise
