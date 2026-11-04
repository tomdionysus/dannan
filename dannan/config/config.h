//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// A node binary's settings, read once at start from layers, each overriding
// the one before: the binary's defaults, a YAML configuration file,
// secrets.yaml beside it, the environment, then the command line. One
// reader for every node binary in the stack (dannan-node, lugh-node,
// lir-node, ...; Tom, 2026-11-04: lifted from Lugh's, on yaml-cpp). A
// binary declares its settings; the reader layers them and says where each
// value came from; the binary reads them typed, and a value that cannot be
// used is refused in the same words whichever layer gave it.
namespace dannan::config {

// A setting was given that cannot be used, or a layer could not be read:
// the message names the source (file, variable or flag) and the key.
class ConfigError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The process environment, from outside, so a reader is tested without
// setting variables on the test process.
class Environment {
 public:
  virtual ~Environment() = default;
  // Nothing when unset; set but empty is an empty string.
  virtual std::optional<std::string> get(const std::string& name) const = 0;
};
const Environment& process_environment() noexcept;

// One setting a binary takes.
struct Setting {
  enum class Kind : uint8_t {
    value,   // one scalar
    list,    // scalars: a YAML list; a repeated flag; a variable split on
             // commas and whitespace
    toggle,  // true or false; a flag with no value sets it true
  };
  // Dotted, as the file nests it: "server.port" is `server: {port: ...}`.
  std::string key;
  Kind kind = Kind::value;
  // "--port"; empty: not on the command line.
  std::string flag;
  // "LUGH_PORT"; empty: not from the environment.
  std::string variable;
  // What it is, for usage and refusals: "a port, 0 to 65535".
  std::string what;
};

// A setting's value, with the layer that gave it: the file's path, the
// variable's name or the flag.
struct Value {
  std::vector<std::string> items;
  std::string source;
};

// What the layers said. Only settings some layer gave are present.
class Settings {
 public:
  // The configuration file read, and secrets.yaml beside it; empty when
  // none was.
  std::string path;
  std::string secrets_path;
  std::map<std::string, Value> values;
  // Command-line arguments that are not flags (a command, say), in order.
  std::vector<std::string> positional;
  // Keys in a file that no setting reads: most likely a misspelling.
  std::vector<std::string> warnings;

  const Value* find(const std::string& key) const;
  // Typed reads: nothing when no layer gave the setting. Throw ConfigError,
  // "<source>: <key> is <what>, not "<text>"", for one that cannot be used.
  std::optional<std::string> text(const std::string& key) const;
  // A non-empty string.
  std::optional<std::string> path_of(const std::string& key) const;
  std::optional<uint64_t> number(const std::string& key, uint64_t least, uint64_t most) const;
  std::optional<bool> toggle(const std::string& key) const;
  std::vector<std::string> list(const std::string& key) const;
  // One of `choices`.
  std::optional<std::string> choice(const std::string& key, const std::vector<std::string>& choices) const;
  // The refusal for `key`'s value, for a binary's own types (an address, a
  // network): "<source>: <key> is <what>, not "<text>"".
  ConfigError refuse(const std::string& key, const std::string& what) const;
};

struct Reader {
  std::vector<Setting> settings;
  // Names the file when the command line does not (--config PATH, -c PATH);
  // empty: only the command line can.
  std::string config_variable;
  // Read when neither names a file, and only if it exists; empty: none.
  std::string default_path;
  const Environment* environment = &process_environment();

  // Layers the settings from `arguments` (argv without the program name).
  // --name value and --name=value both work. Throws ConfigError for a file
  // that is named but missing, or cannot be parsed; a value of the wrong
  // shape; an unknown flag, or one missing its value.
  Settings read(const std::vector<std::string>& arguments) const;
  // The flags, their settings and variables, for a usage text.
  std::string describe() const;
};

}  // namespace dannan::config
