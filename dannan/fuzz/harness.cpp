//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "dannan/fuzz/fuzz.h"

namespace dannan::fuzz {
namespace {

// SplitMix64, as the simulation harness's Random: the standard library's engines and
// distributions differ between libc++ and libstdc++.
uint64_t next(uint64_t& state) noexcept {
  uint64_t z = (state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

size_t below(uint64_t& state, size_t bound) noexcept { return bound ? static_cast<size_t>(next(state) % bound) : 0; }

}  // namespace

std::vector<Target>& targets() {
  static std::vector<Target> all;
  return all;
}

net::Bytes mutate(net::Bytes input, uint64_t& state) {
  const auto edits = 1 + below(state, 4);
  for (size_t e = 0; e < edits; ++e) {
    switch (below(state, 6)) {
      case 0:  // flip a bit
        if (!input.empty()) input[below(state, input.size())] ^= static_cast<uint8_t>(1u << below(state, 8));
        break;
      case 1:  // set a byte to an edge value
        if (!input.empty()) input[below(state, input.size())] = std::array<uint8_t, 4>{0x00, 0x01, 0x7f, 0xff}[below(state, 4)];
        break;
      case 2:  // insert
        input.insert(input.begin() + static_cast<std::ptrdiff_t>(below(state, input.size() + 1)), static_cast<uint8_t>(next(state)));
        break;
      case 3:  // remove
        if (!input.empty()) input.erase(input.begin() + static_cast<std::ptrdiff_t>(below(state, input.size())));
        break;
      case 4:  // truncate
        input.resize(below(state, input.size() + 1));
        break;
      default:  // a length field at its limit
        if (input.size() >= 4) {
          const auto at = below(state, input.size() - 3);
          for (size_t i = 0; i < 4; ++i) input[at + i] = 0xff;
        }
        break;
    }
  }
  return input;
}

size_t smoke(const Target& target, const SmokeOptions& options) {
  const auto seeds = target.seeds();
  if (seeds.empty()) throw std::invalid_argument("fuzz target " + target.name + " gives no seeds");
  uint64_t state = options.seed;
  size_t ran = 0;
  for (const auto& seed : seeds) {
    target.run(seed);
    ++ran;
  }
  for (size_t i = 0; i < options.mutations; ++i) {
    target.run(mutate(seeds[i % seeds.size()], state));
    ++ran;
  }
  for (size_t i = 0; i < options.noise; ++i) {
    net::Bytes input(below(state, options.max_noise + 1));
    for (auto& byte : input) byte = static_cast<uint8_t>(next(state));
    target.run(input);
    ++ran;
  }
  target.run({});
  return ran + 1;
}

void run_all(const std::vector<Target>& targets, std::span<const uint8_t> input) {
  for (const auto& target : targets) target.run(input);
}

size_t replay(const std::vector<Target>& targets, int argc, char** argv) {
  size_t runs = 0;
  const auto run_file = [&](const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const net::Bytes data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    run_all(targets, data);
    ++runs;
  };
  if (argc <= 1)
    for (const auto& target : targets)
      for (const auto& seed : target.seeds()) {
        run_all(targets, seed);
        ++runs;
      }
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path path(argv[i]);
    if (std::filesystem::is_directory(path)) {
      for (const auto& entry : std::filesystem::directory_iterator(path))
        if (entry.is_regular_file()) run_file(entry.path());
    } else {
      run_file(path);
    }
  }
  return runs;
}

}  // namespace dannan::fuzz
