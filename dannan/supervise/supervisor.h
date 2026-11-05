//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dannan/sched/retry.h"
#include "dannan/supervise/subsystem.h"
#include "dannan/supervise/supervised.h"

namespace dannan::supervise {

struct SubsystemStatus {
  std::string name;
  SubsystemState state = SubsystemState::unavailable;
  size_t restart_count = 0;
  std::string last_fault;
};

using SubsystemFactory = std::function<std::unique_ptr<Subsystem>(const SubsystemContext&)>;

struct SupervisorOptions {
  // Where the lifecycle threads' faults are recorded; none, nowhere readable.
  SupervisedThreads* threads = nullptr;
  // Where the plugins and their faults are journalled; none, nowhere.
  std::shared_ptr<status::LogSink> log;
  // Where plugins (.so, .dylib) are found; a missing directory loads none.
  std::filesystem::path plugin_dir;
  // The hosted system's names: the entry symbol its plugins export, and the
  // build identity they must carry.
  std::string entry_symbol = "dannan_subsystem_entry";
  std::string build_identity;
  sched::RetryPolicy policy;
  const contract::TimeSource* time = &contract::steady_time_source();
};

// Owns every subsystem's lifecycle, plugin or built in: loads plugins and
// checks each build identity before calling in, then runs construction,
// start and any fault reported later under the retry policy, disabling a
// subsystem that fails too often. A failure degrades that subsystem; the
// process stays up. One lifecycle thread per subsystem.
class Supervisor {
 public:
  explicit Supervisor(SupervisorOptions options);
  ~Supervisor();
  Supervisor(const Supervisor&) = delete;
  Supervisor& operator=(const Supervisor&) = delete;

  // A subsystem linked into the binary, supervised like a plugin. Before
  // start(); `name` is what Status reports.
  void add_builtin(std::string name, SubsystemFactory factory);
  // Discovers the plugins and starts every lifecycle thread.
  void start(SubsystemContext context);
  // Stops every subsystem and joins the threads; a blocking construction is
  // cancelled through its stop token.
  void stop();
  std::vector<SubsystemStatus> statuses() const;

 private:
  struct Entry;
  void discover();
  void run(Entry& entry, std::stop_token stop);

  const SupervisorOptions _options;
  const status::ScopedLogger _log;
  SubsystemContext _context;
  std::vector<std::unique_ptr<Entry>> _entries;
};

}  // namespace dannan::supervise
