//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "dannan/net/codec.h"

// Fuzz targets: one per parser of untrusted bytes (CONTRIBUTING.md,
// "Layout"). A target takes any bytes and must return or throw only the
// exceptions its parser documents; anything else (a crash, another
// exception, a sanitizer report) is a finding. Each target also gives seeds,
// valid inputs a mutator starts from to reach past the first check.
//
// Built three ways: as a libFuzzer binary (DANNAN_BUILD_FUZZING with a clang
// that has libFuzzer), as a replay driver over files (DANNAN_BUILD_FUZZING
// without one), and into the test binary's smoke test, which runs every
// target over thousands of mutated seeds on each gate.
namespace dannan::fuzz {

struct Target {
  std::string name;
  std::function<void(std::span<const uint8_t>)> run;
  std::function<std::vector<net::Bytes>()> seeds;
};

std::vector<Target>& targets();

struct Registration {
  Registration(std::string name, std::function<void(std::span<const uint8_t>)> run, std::function<std::vector<net::Bytes>()> seeds) {
    targets().push_back({std::move(name), std::move(run), std::move(seeds)});
  }
};

}  // namespace dannan::fuzz
