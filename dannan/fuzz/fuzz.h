//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "dannan/net/codec.h"

// Fuzz targets: one per parser of untrusted bytes (CONTRIBUTING.md, "Layout"). A target takes
// any bytes and must return or throw only the exceptions its parser documents; anything else (a
// crash, another exception, a sanitizer report) is a finding. Each target also gives seeds,
// valid inputs a mutator starts from to reach past the first check.
//
// Exported with dannan::testing (TODO/2026-11-20-stack-plan.md, item 17), so a hosted system
// registers its own targets and runs them the same three ways: as a libFuzzer binary, as a replay
// driver over files, and through smoke() in its own test binary on each gate, with no clang. A
// fuzz binary is two lines:
//
//   int main(int argc, char** argv) { dannan::fuzz::replay(dannan::fuzz::targets(), argc, argv); }
//   extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
//     dannan::fuzz::run_all(dannan::fuzz::targets(), {d, n}); return 0; }
namespace dannan::fuzz {

struct Target {
  std::string name;
  std::function<void(std::span<const uint8_t>)> run;
  std::function<std::vector<net::Bytes>()> seeds;
};

// Every target registered in this binary.
std::vector<Target>& targets();

struct Registration {
  Registration(std::string name, std::function<void(std::span<const uint8_t>)> run, std::function<std::vector<net::Bytes>()> seeds) {
    targets().push_back({std::move(name), std::move(run), std::move(seeds)});
  }
};

// One to four edits of `input`: a bit flipped, a byte set to an edge value, a byte inserted or
// removed, the input truncated, or four bytes set to 0xff (a length at its limit). Every draw is
// SplitMix64 from `state`, which moves on, so a seed gives the same edits on any standard library.
net::Bytes mutate(net::Bytes input, uint64_t& state);

struct SmokeOptions {
  uint64_t seed = 0x0dda'2026;
  // Mutations of the seeds, taken in turn.
  size_t mutations = 3000;
  // Random inputs, each at most max_noise bytes.
  size_t noise = 500;
  size_t max_noise = 256;
};

// Runs `target` over its seeds, then `mutations` mutated seeds, then `noise` random inputs, then
// the empty input; how many inputs ran. Whatever the target throws goes out of smoke(): the
// finding. Throws std::invalid_argument for a target with no seeds.
size_t smoke(const Target& target, const SmokeOptions& options = {});

// Each input through every target.
void run_all(const std::vector<Target>& targets, std::span<const uint8_t> input);

// A replay driver: each argument is a file, or a directory whose files are each an input; with
// no argument, every target's seeds. How many inputs ran.
size_t replay(const std::vector<Target>& targets, int argc, char** argv);

}  // namespace dannan::fuzz
