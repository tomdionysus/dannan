//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/supervise/supervisor.h"

#include <dlfcn.h>

#include <condition_variable>
#include <mutex>
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

// What a subsystem's fault sink reaches: outlives the instance, so a sink it
// kept past its stop finds nobody rather than a freed entry.
struct SinkTarget {
  std::mutex mutex;
  Supervisor::Entry* entry = nullptr;
};

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
  std::string path;
  std::jthread lifecycle;
};

Supervisor::Supervisor(SupervisorOptions options) : _options(std::move(options)), _log(_options.log ? _options.log : status::null_sink(), "subsystem") {}

Supervisor::~Supervisor() { stop(); }

void Supervisor::add_builtin(std::string name, SubsystemFactory factory) {
  if (!factory) return;
  auto entry = std::make_unique<Entry>();
  entry->name = std::move(name);
  entry->path = "built in";
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
  const std::filesystem::directory_iterator listing(_options.plugin_dir, error);
  if (error) _log.warn("subsystem plugin discovery failed dir=" + _options.plugin_dir.string() + " error=" + error.message());
  for (const auto& candidate : listing) {
    const auto extension = candidate.path().extension();
    if (!candidate.is_regular_file() || (extension != ".so" && extension != ".dylib")) continue;
    auto entry = std::make_unique<Entry>();
    entry->name = candidate.path().stem().string();
    const auto refuse = [&](std::string why) {
      contract::Lock lock(entry->mutex);
      entry->state = SubsystemState::disabled;
      entry->last_fault = std::move(why);
    };
    entry->path = candidate.path().string();
    entry->handle = ::dlopen(candidate.path().c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!entry->handle) {
      const std::string why = ::dlerror();
      _log.error("subsystem plugin '" + entry->name + "' failed to load: " + why);
      refuse("failed to load: " + why);
      _entries.push_back(std::move(entry));
      continue;
    }
    ::dlerror();
    auto* symbol = ::dlsym(entry->handle, _options.entry_symbol.c_str());
    const char* symbol_error = ::dlerror();
    if (symbol_error || !symbol) {
      // Not a plugin of this system: a directory may hold other libraries.
      // None of its code was called, so it may be unloaded.
      _log.debug("subsystem plugin candidate '" + entry->name + "' has no entry symbol '" + _options.entry_symbol + "', skipping as not a " +
                 _options.system_name + (_options.system_name.empty() ? "" : " ") + "plugin: " + (symbol_error ? symbol_error : "symbol resolved to null"));
      ::dlclose(entry->handle);
      continue;
    }
    const auto* plugin = reinterpret_cast<PluginEntryFunction>(symbol)();
    if (!plugin) {
      _log.error("subsystem plugin '" + entry->name + "' entry function returned null");
      refuse("its entry function returned null");
    } else if (plugin->build_identity != _options.build_identity) {
      _log.error("subsystem plugin '" + entry->name + "' build identity mismatch: plugin=" + std::string(plugin->build_identity) +
                 " core=" + _options.build_identity + "; refusing to load (partial deploy?)");
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
    std::shared_ptr<SinkTarget> sink_target;
    const auto disown_sink = [&sink_target] {
      if (!sink_target) return;
      std::lock_guard held(sink_target->mutex);
      sink_target->entry = nullptr;
    };
    bool declined = false;
    std::string fault;
    try {
      auto attempt = _context;
      attempt.startup_stop = stop;
      instance = entry.factory(attempt);
      if (instance) {
        // Before start(): threads started inside it may report.
        sink_target = std::make_shared<SinkTarget>();
        sink_target->entry = &entry;
        instance->attach_fault_sink([target = sink_target](std::string reason) {
          std::lock_guard held(target->mutex);
          if (!target->entry) return;  // that instance is gone: a sink kept past its stop is ignored
          auto& reported = *target->entry;
          contract::Lock lock(reported.mutex);
          if (reported.fault_requested || reported.stopping) return;  // first reason wins
          reported.fault_requested = true;
          reported.pending_fault = reason.empty() ? std::string("subsystem reported a fault") : std::move(reason);
          reported.changed.notify_all();
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
      _log.info("subsystem plugin '" + entry.name + "' loaded but its capability is not enabled on this node path=" + entry.path);
      contract::Lock lock(entry.mutex);
      entry.state = SubsystemState::unavailable;
      return;
    }
    if (fault.empty()) {
      _log.info("subsystem plugin '" + entry.name + "' loaded and running path=" + entry.path);
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
        } catch (const std::exception& error) {
          _log.warn("subsystem '" + entry.name + "' stop() threw: " + error.what());
        } catch (...) {
          _log.warn("subsystem '" + entry.name + "' stop() threw an unknown exception");
        }
      }
      // Destroyed before a replacement is built.
      owned.reset();
      disown_sink();
      if (!faulted) return;
      _log.error("subsystem '" + entry.name + "' faulted while running: " + fault);
    } else {
      _log.error("subsystem '" + entry.name + "' failed to start: " + fault);
      instance.reset();
      disown_sink();
    }
    {
      contract::Lock lock(entry.mutex);
      entry.last_fault = fault;
      ++entry.restart_count;
    }
    const auto delay = retry.failed(_options.policy, _options.time->now());
    if (!delay)
      _log.error("subsystem '" + entry.name + "' disabled after " + std::to_string(retry.failures_in_window()) + " failed attempts; needs an operator");
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
  _entries.clear();
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
