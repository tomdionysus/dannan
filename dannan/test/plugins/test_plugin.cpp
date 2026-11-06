//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Test plugins for the supervisor, one behaviour each, chosen at build time
// by DANNAN_TEST_PLUGIN_KIND: running, writes_on_stop, declining, faulting
// (create throws), faulting_after_start, mismatched (another build
// identity), no_entry (no entry symbol at all).

#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "dannan/supervise/subsystem.h"

namespace {

using dannan::supervise::Subsystem;
using dannan::supervise::SubsystemContext;

constexpr std::string_view kind = DANNAN_TEST_PLUGIN_KIND;

class TestSubsystem final : public Subsystem {
 public:
  explicit TestSubsystem(const SubsystemContext& context) : _context(context) {}
  std::string_view name() const noexcept override { return kind; }
  void attach_fault_sink(FaultSink sink) override { _sink = std::move(sink); }
  void start() override {
    if (kind == "faulting_after_start") _sink("lost its disk");
  }
  void stop() override {
    if (kind == "writes_on_stop") {
      const auto* path = std::any_cast<std::string>(&_context.host);
      if (path) std::ofstream(*path) << "stopped cleanly\n";
    }
  }

 private:
  SubsystemContext _context;
  FaultSink _sink;
};

std::unique_ptr<Subsystem> create(const SubsystemContext& context) {
  if (kind == "declining") return nullptr;
  if (kind == "faulting") throw std::runtime_error("cannot open its database");
  return std::make_unique<TestSubsystem>(context);
}

const dannan::supervise::PluginEntry entry{kind == "mismatched" ? "some other build" : "dannan test build", &create};

}  // namespace

#ifndef DANNAN_TEST_PLUGIN_NO_ENTRY
extern "C" const dannan::supervise::PluginEntry* dannan_test_subsystem_entry() {
  return std::string_view(DANNAN_TEST_PLUGIN_KIND) == "null_entry" ? nullptr : &entry;
}
#endif
