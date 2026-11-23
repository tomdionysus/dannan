//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// The configuration reader every node binary shares: layered settings, typed reads, one refusal.

#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dannan::config {

/// A setting was given that cannot be used, or a layer could not be read. The message names the
/// source (the file, the variable or the flag) and the key.
class ConfigError : public std::runtime_error {
 public:
  /// With the message, naming the source and the key.
  using std::runtime_error::runtime_error;
};

/// The process environment, from outside, so a reader is tested without setting variables on the
/// test process.
class Environment {
 public:
  virtual ~Environment() = default;
  /// A variable's value: nothing when unset; set but empty is an empty string.
  virtual std::optional<std::string> get(const std::string& name) const = 0;
};
/// The real process environment.
const Environment& process_environment() noexcept;

/// One setting a binary takes.
struct Setting {
  /// Its shape.
  enum class Kind : uint8_t {
    /// One scalar.
    value,
    /// Scalars: a YAML list, a repeated flag, or a variable split on commas and whitespace.
    list,
    /// True or false; a flag with no value sets it true.
    toggle,
  };
  /// Dotted, as the file nests it: "server.port" is `server: {port: ...}`. The key is the source of
  /// truth; the flag and variable are made from it (docs/config.md).
  std::string key;
  /// Its shape.
  Kind kind = Kind::value;
  /// "--server-port"; empty: not on the command line.
  std::string flag;
  /// "LUGH_SERVER_PORT"; empty: not from the environment.
  std::string variable;
  /// What it is, for usage and refusals: "a port, 0 to 65535".
  std::string what;
};

/// A setting's value, with the layer that gave it.
struct Value {
  /// Its items: one for a value or toggle, any number for a list.
  std::vector<std::string> items;
  /// Where it came from: the file's path, the variable's name or the flag.
  std::string source;
};

/// What the layers said. Only settings some layer gave are present, so a binary tells "not given"
/// from a default.
class Settings {
 public:
  /// The configuration file read; empty when none was.
  std::string path;
  /// secrets.yaml beside it, when read; empty when none was.
  std::string secrets_path;
  /// Every setting given, by key.
  std::map<std::string, Value> values;
  /// Command-line arguments that are not flags (a command word, say), in order.
  std::vector<std::string> positional;
  /// Keys in a file that no setting reads: most likely a misspelling.
  std::vector<std::string> warnings;

  /// The value given for `key`; null when no layer gave it.
  const Value* find(const std::string& key) const;
  /// A setting as text; nothing when no layer gave it.
  /// @throws ConfigError for a value given as more than one item. Every typed read below reads
  ///   through this, so throws the same.
  std::optional<std::string> text(const std::string& key) const;
  /// A setting as a non-empty path.
  /// @throws ConfigError for an empty one.
  std::optional<std::string> path_of(const std::string& key) const;
  /// A setting as a whole number from `least` to `most`.
  /// @throws ConfigError for any other text, naming the source, the key, what it should be and the
  ///   text given.
  std::optional<uint64_t> number(const std::string& key, uint64_t least, uint64_t most) const;
  /// A setting as true or false (true, false, yes, no, on, off, 1, 0).
  /// @throws ConfigError for any other text.
  std::optional<bool> toggle(const std::string& key) const;
  /// A list setting's items; empty when not given.
  std::vector<std::string> list(const std::string& key) const;
  /// A setting that must be one of `choices`.
  /// @throws ConfigError for any other text.
  std::optional<std::string> choice(const std::string& key, const std::vector<std::string>& choices) const;
  /// The refusal for `key`'s value, for a binary's own types (an address, a network), in the same
  /// words every refusal uses: the source, then "key is what, not" the text given in quotes.
  ConfigError refuse(const std::string& key, const std::string& what) const;
};

/// A node binary's settings reader (one for every node binary in the stack, lifted from Lugh's).
///
/// Settings are read once at start from layers, each overriding the one before: the binary's
/// defaults, a YAML configuration file, secrets.yaml beside it, the environment, then the command
/// line. The binary declares its settings; the reader layers them and says where each value came
/// from; the binary reads them typed, and a value that cannot be used is refused in the same words
/// whichever layer gave it. docs/config.md has the naming rule every binary follows: a flag and a
/// variable made from the YAML key.
///
/// @code
/// config::Reader reader;
/// config::add_node_settings(reader, "MIMIR");
/// reader.settings.push_back({"resp.port", config::Setting::Kind::value, "--resp-port", "MIMIR_RESP_PORT", "a port"});
/// reader.config_variable = "MIMIR_CONFIG";
/// const auto settings = reader.read({argv + 1, argv + argc});  // throws ConfigError
/// const auto port = settings.number("resp.port", 0, 65535).value_or(6379);
/// @endcode
struct Reader {
  /// The settings the binary takes.
  std::vector<Setting> settings;
  /// The variable that names the file when the command line does not (--config PATH, -c PATH);
  /// empty: only the command line can.
  std::string config_variable;
  /// Read when neither names a file, and only if it exists; empty: none.
  std::string default_path;
  /// Where variables are read; the process's by default.
  const Environment* environment = &process_environment();

  /// Layers the settings from `arguments` (argv without the program name). --name value and
  /// --name=value both work.
  /// @throws ConfigError for a file that is named but missing or cannot be parsed, a value of the
  ///   wrong shape, an unknown flag, or one missing its value.
  Settings read(const std::vector<std::string>& arguments) const;
  /// The flags, their settings and variables, for a usage text.
  std::string describe() const;
};

}  // namespace dannan::config
