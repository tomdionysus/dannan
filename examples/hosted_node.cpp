//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A hosted system on its Context, as a node binary (docs/guide/settings.md, docs/guide/nodes.md):
// the node-program helper for main; a setting of the system's own; its message type registered
// before the node starts; its health pushed as a check; a channel; a drain step; and a status line
// with its own fields. With no state given it makes a throwaway cluster of one, so it runs alone.
//
//   hosted_node [--node-seconds N] [--cluster-state DIR --cluster-key FILE] [--greeting TEXT]
//
// It prints one JSON line when started and one when stopped, and exits 0.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "dannan/identity/cluster_key.h"
#include "dannan/supervise/node_program.h"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr dannan::net::MessageType greet{1500};

// The hosted system: everything it does, it does through the Context.
class Greeter {
 public:
  Greeter(dannan::Context& context, std::string greeting) : _context(context), _greeting(std::move(greeting)), _log(context.logger("greeter")) {}

  void start() {
    // Answer the type registered before start.
    _context.routes().bind(greet, [this](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message&) {
      ++_answered;
      return dannan::net::Handled::processed({dannan::net::message::ok, dannan::net::Bytes(_greeting.begin(), _greeting.end()), {}});
    });
    // A channel of its own, to tell its peers it started.
    _context.channels().open("greeter.events", dannan::net::Delivery::notify);
    _context.channels().publish("greeter.events", "started", {});
    // Its health: pushed, with how long the verdict stands, so a hung loop reads as not ready.
    _context.readiness().set("greeter.ready", dannan::status::Health::ready, {}, 30s);
    _log.info("greeting with \"" + _greeting + "\"");
  }

  // Its drain: no new greetings, then done.
  void drain(std::function<void()> done) {
    _context.routes().unbind(greet);
    done();
  }

  uint64_t answered() const { return _answered.load(); }

 private:
  dannan::Context& _context;
  const std::string _greeting;
  dannan::status::ScopedLogger _log;
  std::atomic<uint64_t> _answered{0};
};

}  // namespace

int main(int argc, char** argv) {
  dannan::supervise::ProgramSpec spec;
  spec.name = "hosted-node";
  spec.prefix = "HOSTED";
  spec.version = "1.0.0";
  // Its own setting, beside cluster.*, log.* and node.*: --greeting, HOSTED_GREETING, or greeting:.
  spec.settings = [](dannan::config::Reader& reader) {
    reader.settings.push_back({"greeting", dannan::config::Setting::Kind::value, "--greeting", "HOSTED_GREETING", "what it answers with"});
  };
  // No state given: a throwaway cluster of one, its state and key made for the run, removed at exit.
  const fs::path throwaway = fs::temp_directory_path() / "dannan-example-hosted-node";
  spec.configure = [&](const dannan::config::Settings& settings, dannan::NodeConfig& config) {
    if (!config.state_path.empty()) return;
    fs::remove_all(throwaway);
    fs::create_directories(throwaway);
    std::ofstream(throwaway / "cluster.key") << "a throwaway cluster's key, made for this run";
    config.state_path = throwaway / "state";
    config.key = dannan::identity::load_cluster_key(throwaway / "cluster.key");
    config.listen_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.port = 0;
    (void)settings;
  };
  // Its message type, registered before the node starts: routes freeze there.
  spec.prepare = [](dannan::Node& node) { node.context().routes().add(greet, {"greet", dannan::net::Lanes::control, dannan::net::Frames::any, false}); };

  std::unique_ptr<Greeter> greeter;
  return dannan::supervise::run_node_program(argc, argv, spec, [&](dannan::supervise::Program& program) {
    greeter = std::make_unique<Greeter>(program.context(), program.settings().text("greeting").value_or("hello"));
    greeter->start();
    program.on_drain([&](dannan::contract::Deadline, std::function<void()> done) { greeter->drain(std::move(done)); });
    program.on_status([&](dannan::json::object& line) { line["answered"] = greeter->answered(); });
    program.on_exit([&] {
      dannan::json::object stopped;
      stopped["event"] = "stopped";
      program.say(stopped);
      fs::remove_all(throwaway);
    });
    dannan::json::object started;
    started["event"] = "started";
    started["port"] = program.node().port();
    program.say(started);
  });
}
