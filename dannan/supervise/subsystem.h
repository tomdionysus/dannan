//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <any>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

#include "dannan/contract/time_source.h"

namespace dannan::supervise {

// What the supervisor says of each subsystem; held by the supervisor, not
// the instance, which may be gone when asked.
enum class SubsystemState : uint8_t {
  unavailable,  // declined: not run on this node
  starting,
  running,
  faulted,  // the last attempt failed; it will be retried
  restarting,
  disabled,  // failed too often, or refused at load: needs an operator
};

const char* state_name(SubsystemState state) noexcept;

// What a subsystem is built with. `host` is the hosted system's own
// context, opaque to Dannan (any_cast it to what was passed to start()).
struct SubsystemContext {
  std::any host;
  const contract::TimeSource* time = &contract::steady_time_source();
  // Cancels a construction that may block (waiting for the first namespace,
  // say); the lifecycle thread's token, per attempt.
  std::stop_token startup_stop;
};

// A subsystem run by the Supervisor, as a plugin or built in. A faulted
// instance is destroyed and a fresh one built in its place.
class Subsystem {
 public:
  // Reports a fault found after start(); the reason becomes the status's
  // last fault.
  using FaultSink = std::function<void(std::string reason)>;

  virtual ~Subsystem() = default;
  virtual std::string_view name() const noexcept = 0;
  virtual void start() = 0;
  // May write: it runs before the instance is destroyed.
  virtual void stop() = 0;
  // Installed between construction and start(). Calling it makes the
  // supervisor destroy this instance and try a fresh one under the retry
  // policy. Callable from any of the subsystem's threads, start() included;
  // the first reason wins and calls once stopping has begun are ignored.
  virtual void attach_fault_sink(FaultSink sink) { (void)sink; }
};

// What a plugin library exports, as one flat C symbol returning this (the
// symbol's name is the hosted system's: SupervisorOptions::entry_symbol).
// Core and plugins are built together from one commit, so a C++ interface
// across dlopen is safe once the build identity matches.
struct PluginEntry {
  // Must equal the loading build's identity; a mismatch (a partial deploy)
  // is refused before any other call in.
  std::string_view build_identity;
  // Null: not run on this node (unavailable, never retried). A throw: a
  // failure, under backoff and disable.
  std::unique_ptr<Subsystem> (*create)(const SubsystemContext&);
};

using PluginEntryFunction = const PluginEntry* (*)();

}  // namespace dannan::supervise
