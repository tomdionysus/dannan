//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/linearizable.h"

// Models the checker is checked with and agreement's simulations use; a hosted system writes
// its own (Mimir's commands, Kvasir's ranges) the same way. Header-side, as the design has it.
namespace dannan::test::sim {

// Registers over every key of a group. An operation's input is 'w' and a value, written to
// each of its keys at once, or 'r', answered with its keys' values joined by ','. A key never
// written reads as empty.
class RegisterModel final : public Model {
 public:
  static Bytes write(const std::string& value) {
    Bytes out{'w'};
    out.insert(out.end(), value.begin(), value.end());
    return out;
  }
  static Bytes read() { return Bytes{'r'}; }
  static Bytes values(const std::vector<std::string>& values) {
    std::string joined;
    for (size_t i = 0; i < values.size(); ++i) joined += (i ? "," : "") + values[i];
    return Bytes(joined.begin(), joined.end());
  }

  std::string initial(const std::vector<std::string>&) const override { return encode({}); }
  std::vector<std::string> step(const std::string& state, const Operation& operation, const Bytes* result) const override {
    auto held = decode(state);
    if (operation.input.empty()) return {};
    if (operation.input[0] == 'w') {
      const std::string value(operation.input.begin() + 1, operation.input.end());
      for (const auto& key : operation.keys) held[key] = value;
      return {encode(held)};
    }
    std::vector<std::string> now;
    for (const auto& key : operation.keys) now.push_back(held.count(key) ? held.at(key) : std::string());
    if (result && *result != values(now)) return {};
    return {state};
  }

 private:
  static std::string encode(const std::map<std::string, std::string>& held) {
    std::string out;
    for (const auto& [key, value] : held) out += key + '\x1f' + value + '\x1e';
    return out;
  }
  static std::map<std::string, std::string> decode(const std::string& state) {
    std::map<std::string, std::string> out;
    size_t at = 0;
    while (at < state.size()) {
      const auto unit = state.find('\x1f', at);
      const auto record = state.find('\x1e', unit);
      out[state.substr(at, unit - at)] = state.substr(unit + 1, record - unit - 1);
      at = record + 1;
    }
    return out;
  }
};

// A counter per key: 'i' increments and is answered with the new value, 'r' reads it; values
// are eight bytes big-endian, as agreement's simulations encode them.
class CounterModel final : public Model {
 public:
  static Bytes number(uint64_t value) {
    Bytes out(8);
    for (int i = 0; i < 8; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(value >> (56 - 8 * i));
    return out;
  }

  std::string initial(const std::vector<std::string>&) const override { return "0"; }
  std::vector<std::string> step(const std::string& state, const Operation& operation, const Bytes* result) const override {
    const uint64_t now = std::stoull(state);
    if (operation.input == Bytes{'i'}) {
      if (result && *result != number(now + 1)) return {};
      return {std::to_string(now + 1)};
    }
    if (result && *result != number(now)) return {};
    return {state};
  }
};

}  // namespace dannan::test::sim
