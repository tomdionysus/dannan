//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A node binary made of the node-program helper alone (item 14), for its tests: each hook says
// what it did as a line on stdout, so a test reads the order things happened in.
//
//   probe.drain_ms   how long its drain step takes before it calls done; "never": it does not
//   probe.before     "exit3": before_start ends the program with 3
//   command "hello"  prints hello and exits 7

#include <cstdio>
#include <thread>

#include "dannan/supervise/node_program.h"

int main(int argc, char** argv) {
  using namespace dannan;
  supervise::ProgramSpec spec;
  spec.name = "probe-node";
  spec.prefix = "PROBE";
  spec.version = "9.9.9";
  spec.settings = [](config::Reader& reader) {
    reader.settings.push_back({"probe.drain_ms", config::Setting::Kind::value, "--probe-drain-ms", "PROBE_PROBE_DRAIN_MS", "ms, or never"});
    reader.settings.push_back({"probe.before", config::Setting::Kind::value, "--probe-before", "", "exit3"});
  };
  spec.before_start = [](const config::Settings& settings, NodeConfig&) -> std::optional<int> {
    if (settings.text("probe.before").value_or("") == "exit3") return 3;
    return std::nullopt;
  };
  spec.commands["hello"] = [](const config::Settings&) {
    std::printf("hello\n");
    return 7;
  };
  spec.prepare = [](Node&) {
    std::printf("{\"event\":\"probe.prepared\"}\n");
    std::fflush(stdout);
  };
  return supervise::run_node_program(argc, argv, spec, [](supervise::Program& program) {
    const auto drain = program.settings().text("probe.drain_ms").value_or("0");
    const auto said = [&program](const char* event) {
      json::object line;
      line["event"] = event;
      program.say(line);
    };
    program.on_drain([said, drain](contract::Deadline, std::function<void()> done) {
      said("probe.drain");
      if (drain == "never") return;
      std::thread([done, ms = std::stoi(drain)] {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        done();
      }).detach();
    });
    program.on_stop([said] { said("probe.stop"); });
    program.on_exit([said] { said("probe.exit"); });
    program.on_reload([said] { said("probe.reload"); });
    program.on_status([](json::object& line) { line["probe"] = 1; });
    json::object started;
    started["event"] = "probe.started";
    started["port"] = program.node().port();
    program.say(started);
  });
}
