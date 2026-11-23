//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Settings (docs/guide/settings.md): one reader with the node settings and a program's own, read
// from a YAML file, the environment and the command line, each layer over the one before; a typed
// read; a NodeConfig made from them; and a value that cannot be used, refused naming the layer
// that gave it. Exits 0 when all of it happened.
//
//   settings [STATE_DIR]

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include "dannan/config/node_settings.h"

namespace fs = std::filesystem;

namespace {

// An environment of the example's own, so it sets no variable of the process.
class Environment final : public dannan::config::Environment {
 public:
  std::map<std::string, std::string> values;
  std::optional<std::string> get(const std::string& name) const override {
    const auto found = values.find(name);
    if (found == values.end()) return std::nullopt;
    return found->second;
  }
};

}  // namespace

int main(int argc, char** argv) {
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "dannan-example-settings";
  fs::remove_all(root);
  fs::create_directories(root);

  // The file: nested as the keys are dotted.
  std::ofstream(root / "example.yaml") << "cluster:\n"
                                          "  state: "
                                       << (root / "state").string()
                                       << "\n"
                                          "  listen:\n"
                                          "    port: 7437\n"
                                          "example:\n"
                                          "  greeting: hello from the file\n";
  std::ofstream(root / "cluster.key") << "an example cluster's key, made for this run";

  // The reader: the node settings every node binary takes, and the program's own.
  dannan::config::Reader reader;
  dannan::config::add_node_settings(reader, "EXAMPLE");
  reader.settings.push_back({"example.greeting", dannan::config::Setting::Kind::value, "--example-greeting", "EXAMPLE_EXAMPLE_GREETING", "a greeting"});
  reader.config_variable = "EXAMPLE_CONFIG";
  Environment environment;
  environment.values["EXAMPLE_CONFIG"] = (root / "example.yaml").string();
  environment.values["EXAMPLE_CLUSTER_LISTEN_PORT"] = "7500";  // the environment over the file
  environment.values["EXAMPLE_CLUSTER_KEY"] = (root / "cluster.key").string();
  reader.environment = &environment;

  int status = 1;
  try {
    // The command line last, over everything: --name value and --name=value both work.
    const auto settings = reader.read({"--cluster-listen-port=7600", "--example-greeting", "hello from the command line"});
    const auto port = settings.number("cluster.listen.port", 0, 65535);
    std::printf("cluster.listen.port = %llu, from %s\n", static_cast<unsigned long long>(*port), settings.find("cluster.listen.port")->source.c_str());
    std::printf("example.greeting = %s\n", settings.text("example.greeting")->c_str());
    // The node's configuration from the node settings; what none gave keeps its default.
    const auto config = dannan::config::node_config(settings);
    std::printf("NodeConfig: state %s, port %u\n", config.state_path.string().c_str(), config.port);

    // A value that cannot be used is refused in one form, naming the layer that gave it.
    bool refused = false;
    try {
      (void)reader.read({"--cluster-listen-port", "seventy"}).number("cluster.listen.port", 0, 65535);
    } catch (const dannan::config::ConfigError& error) {
      refused = true;
      std::printf("refused: %s\n", error.what());
    }
    status = *port == 7600 && config.port == 7600 && refused ? 0 : 1;
  } catch (const dannan::config::ConfigError& error) {
    std::fprintf(stderr, "settings: %s\n", error.what());
  }
  fs::remove_all(root);
  return status;
}
