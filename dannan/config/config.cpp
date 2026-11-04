//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/config/config.h"

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace dannan::config {

namespace {

class ProcessEnvironment final : public Environment {
 public:
  std::optional<std::string> get(const std::string& name) const override {
    const char* value = std::getenv(name.c_str());
    if (!value) return std::nullopt;
    return std::string(value);
  }
};

const char* kind_name(Setting::Kind kind) {
  switch (kind) {
    case Setting::Kind::value:
      return "a single value";
    case Setting::Kind::list:
      return "a list";
    case Setting::Kind::toggle:
      return "true or false";
  }
  return "a value";
}

std::optional<bool> parse_toggle(const std::string& text) {
  if (text == "true" || text == "yes" || text == "on" || text == "1") return true;
  if (text == "false" || text == "no" || text == "off" || text == "0") return false;
  return std::nullopt;
}

std::vector<std::string> split_list(const std::string& text) {
  std::vector<std::string> out;
  std::string item;
  for (const char c : text) {
    if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
      if (!item.empty()) out.push_back(std::move(item));
      item.clear();
    } else {
      item += c;
    }
  }
  if (!item.empty()) out.push_back(std::move(item));
  return out;
}

class Layering {
 public:
  Layering(const Reader& reader, Settings& out) : _out(out) {
    for (const auto& setting : reader.settings) _by_key.emplace(setting.key, &setting);
  }

  void file(const std::string& path) {
    YAML::Node root;
    try {
      root = YAML::LoadFile(path);
    } catch (const YAML::Exception& error) {
      throw ConfigError(path + ": " + error.what());
    }
    // An empty file is a file of defaults.
    if (!root || root.IsNull()) return;
    if (!root.IsMap()) throw ConfigError(path + ": the top level must be a map");
    walk(root, "", path);
  }

  void set(const Setting& setting, std::vector<std::string> items, const std::string& source) {
    if (setting.kind == Setting::Kind::toggle) {
      if (items.size() != 1 || !parse_toggle(items.front()))
        throw ConfigError(source + ": " + setting.key + " is true or false, not \"" + (items.empty() ? std::string() : items.front()) + "\"");
    }
    _out.values[setting.key] = Value{std::move(items), source};
  }

  const Setting* setting(const std::string& key) const {
    const auto found = _by_key.find(key);
    return found == _by_key.end() ? nullptr : found->second;
  }

 private:
  bool is_section(const std::string& prefix) const {
    const auto dotted = prefix + ".";
    for (const auto& [key, _] : _by_key)
      if (key.rfind(dotted, 0) == 0) return true;
    return false;
  }

  void walk(const YAML::Node& map, const std::string& prefix, const std::string& path) {
    for (const auto& entry : map) {
      const auto name = entry.first.as<std::string>();
      const auto key = prefix.empty() ? name : prefix + "." + name;
      const auto& node = entry.second;
      if (const auto* known = setting(key)) {
        take(*known, node, path);
        continue;
      }
      if (is_section(key)) {
        if (node.IsNull()) continue;
        if (!node.IsMap()) throw ConfigError(path + ": " + key + " must be a map");
        walk(node, key, path);
        continue;
      }
      _out.warnings.push_back(path + ": unknown key " + key + ", not read");
    }
  }

  void take(const Setting& known, const YAML::Node& node, const std::string& path) {
    if (known.kind == Setting::Kind::list) {
      // Null: an empty list, clearing what a layer before gave.
      if (node.IsNull()) return set(known, {}, path);
      if (!node.IsSequence()) throw ConfigError(path + ": " + known.key + " must be a list");
      std::vector<std::string> items;
      for (const auto& item : node) {
        if (!item.IsScalar()) throw ConfigError(path + ": " + known.key + " holds something that is not a single value");
        items.push_back(item.as<std::string>());
      }
      return set(known, std::move(items), path);
    }
    // Null: as if the key were not there.
    if (node.IsNull()) return;
    if (!node.IsScalar()) throw ConfigError(path + ": " + known.key + " must be " + kind_name(known.kind));
    set(known, {node.as<std::string>()}, path);
  }

  Settings& _out;
  std::map<std::string, const Setting*> _by_key;
};

}  // namespace

const Environment& process_environment() noexcept {
  static const ProcessEnvironment environment;
  return environment;
}

const Value* Settings::find(const std::string& key) const {
  const auto found = values.find(key);
  return found == values.end() ? nullptr : &found->second;
}

ConfigError Settings::refuse(const std::string& key, const std::string& what) const {
  const auto* value = find(key);
  std::string text;
  for (const auto& item : value ? value->items : std::vector<std::string>{}) text += (text.empty() ? "" : ",") + item;
  return ConfigError((value ? value->source : std::string("the defaults")) + ": " + key + " is " + what + ", not \"" + text + "\"");
}

std::optional<std::string> Settings::text(const std::string& key) const {
  const auto* value = find(key);
  if (!value) return std::nullopt;
  if (value->items.size() != 1) throw refuse(key, "a single value");
  return value->items.front();
}

std::optional<std::string> Settings::path_of(const std::string& key) const {
  auto out = text(key);
  if (out && out->empty()) throw refuse(key, "a path");
  return out;
}

std::optional<uint64_t> Settings::number(const std::string& key, uint64_t least, uint64_t most) const {
  const auto given = text(key);
  if (!given) return std::nullopt;
  const auto wanted = "a number, " + std::to_string(least) + " to " + std::to_string(most);
  if (given->empty() || given->size() > 20) throw refuse(key, wanted);
  for (const char c : *given)
    if (!std::isdigit(static_cast<unsigned char>(c))) throw refuse(key, wanted);
  uint64_t out = 0;
  try {
    out = std::stoull(*given);
  } catch (const std::exception&) {
    throw refuse(key, wanted);
  }
  if (out < least || out > most) throw refuse(key, wanted);
  return out;
}

std::optional<bool> Settings::toggle(const std::string& key) const {
  const auto given = text(key);
  if (!given) return std::nullopt;
  const auto out = parse_toggle(*given);
  if (!out) throw refuse(key, "true or false");
  return out;
}

std::vector<std::string> Settings::list(const std::string& key) const {
  const auto* value = find(key);
  return value ? value->items : std::vector<std::string>{};
}

std::optional<std::string> Settings::choice(const std::string& key, const std::vector<std::string>& choices) const {
  const auto given = text(key);
  if (!given) return std::nullopt;
  for (const auto& choice : choices)
    if (*given == choice) return given;
  std::string wanted;
  for (size_t i = 0; i < choices.size(); ++i) wanted += (i == 0 ? "" : i + 1 == choices.size() ? " or " : ", ") + choices[i];
  throw refuse(key, wanted);
}

Settings Reader::read(const std::vector<std::string>& arguments) const {
  Settings out;
  Layering layers(*this, out);

  // The command line is read first, so --config is known, and applied last.
  std::string config_flag;
  std::vector<std::pair<const Setting*, std::vector<std::string>>> flags;
  std::map<const Setting*, size_t> list_flags;
  std::vector<std::string> flag_sources;
  for (size_t i = 0; i < arguments.size(); ++i) {
    const auto& argument = arguments[i];
    if (argument.empty() || argument[0] != '-') {
      out.positional.push_back(argument);
      continue;
    }
    auto name = argument;
    std::optional<std::string> inline_value;
    if (const auto equals = argument.find('='); equals != std::string::npos) {
      name = argument.substr(0, equals);
      inline_value = argument.substr(equals + 1);
    }
    const auto value_of = [&](const std::string& what) {
      if (inline_value) return *inline_value;
      if (i + 1 >= arguments.size()) throw ConfigError(name + " needs " + what);
      return arguments[++i];
    };
    if (name == "--config" || name == "-c") {
      config_flag = value_of("a path");
      if (config_flag.empty()) throw ConfigError(name + " needs a path");
      continue;
    }
    const Setting* found = nullptr;
    for (const auto& setting : settings)
      if (!setting.flag.empty() && setting.flag == name) found = &setting;
    if (!found) throw ConfigError("unknown option " + name);
    if (found->kind == Setting::Kind::toggle) {
      flags.push_back({found, {inline_value ? *inline_value : std::string("true")}});
      flag_sources.push_back(name);
    } else if (found->kind == Setting::Kind::list) {
      // A repeated list flag adds to itself, and replaces the layers below.
      const auto item = value_of(found->what.empty() ? "a value" : found->what);
      if (const auto at = list_flags.find(found); at != list_flags.end()) {
        flags[at->second].second.push_back(item);
      } else {
        list_flags.emplace(found, flags.size());
        flags.push_back({found, {item}});
        flag_sources.push_back(name);
      }
    } else {
      flags.push_back({found, {value_of(found->what.empty() ? "a value" : found->what)}});
      flag_sources.push_back(name);
    }
  }

  // Which file: named on the command line or by the variable, it must
  // exist; the default is read only if it does.
  std::string path = config_flag;
  if (path.empty() && !config_variable.empty())
    if (const auto named = environment->get(config_variable); named && !named->empty()) path = *named;
  if (!path.empty()) {
    if (!std::filesystem::is_regular_file(path)) throw ConfigError("no configuration file at " + path);
  } else if (!default_path.empty() && std::filesystem::is_regular_file(default_path)) {
    path = default_path;
  }
  if (!path.empty()) {
    layers.file(path);
    out.path = path;
    // Secrets sit beside the configuration, in the same shape, and win
    // over it.
    const auto secrets = (std::filesystem::path(path).parent_path() / "secrets.yaml").string();
    if (std::filesystem::is_regular_file(secrets)) {
      layers.file(secrets);
      out.secrets_path = secrets;
    }
  }

  for (const auto& setting : settings) {
    if (setting.variable.empty()) continue;
    const auto value = environment->get(setting.variable);
    if (!value) continue;
    layers.set(setting, setting.kind == Setting::Kind::list ? split_list(*value) : std::vector<std::string>{*value}, setting.variable);
  }

  for (size_t i = 0; i < flags.size(); ++i) layers.set(*flags[i].first, std::move(flags[i].second), flag_sources[i]);
  return out;
}

std::string Reader::describe() const {
  std::ostringstream out;
  for (const auto& setting : settings) {
    if (setting.flag.empty() && setting.variable.empty()) continue;
    out << "  " << (setting.flag.empty() ? std::string("(no flag)") : setting.flag);
    if (setting.kind != Setting::Kind::toggle) out << " " << (setting.kind == Setting::Kind::list ? "VALUE..." : "VALUE");
    out << "    " << setting.key;
    if (!setting.what.empty()) out << ": " << setting.what;
    if (!setting.variable.empty()) out << " (" << setting.variable << ")";
    out << "\n";
  }
  return out.str();
}

}  // namespace dannan::config
