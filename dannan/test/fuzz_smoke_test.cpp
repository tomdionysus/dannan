//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Every fuzz target (dannan/fuzz/), on every gate: its seeds, then
// thousands of mutations of them (bytes flipped, inserted, removed,
// truncated, lengths pushed to their limits) and random inputs. A target
// that crashes, or throws what its parser does not document, fails here.
// Deterministic: the same inputs every run, so a failure reproduces.

#include <gtest/gtest.h>

#include <random>

#include "dannan/fuzz/fuzz.h"

namespace dannan::fuzz {
namespace {

net::Bytes mutate(net::Bytes input, std::mt19937& random) {
  const auto pick = [&](size_t bound) { return bound ? std::uniform_int_distribution<size_t>(0, bound - 1)(random) : 0; };
  const auto edits = 1 + pick(4);
  for (size_t e = 0; e < edits; ++e) {
    switch (pick(6)) {
      case 0:  // flip a bit
        if (!input.empty()) input[pick(input.size())] ^= static_cast<uint8_t>(1u << pick(8));
        break;
      case 1:  // set a byte to an edge value
        if (!input.empty()) input[pick(input.size())] = std::array<uint8_t, 4>{0x00, 0x01, 0x7f, 0xff}[pick(4)];
        break;
      case 2:  // insert
        input.insert(input.begin() + static_cast<std::ptrdiff_t>(pick(input.size() + 1)), static_cast<uint8_t>(random()));
        break;
      case 3:  // remove
        if (!input.empty()) input.erase(input.begin() + static_cast<std::ptrdiff_t>(pick(input.size())));
        break;
      case 4:  // truncate
        input.resize(pick(input.size() + 1));
        break;
      case 5:  // a length field at its limit
        if (input.size() >= 4) {
          const auto at = pick(input.size() - 3);
          for (size_t i = 0; i < 4; ++i) input[at + i] = 0xff;
        }
        break;
    }
  }
  return input;
}

TEST(Fuzz, EveryTargetSurvivesMutatedAndRandomInputs) {
  const auto& all = targets();
  ASSERT_GE(all.size(), 7u);
  for (const auto& target : all) {
    SCOPED_TRACE(target.name);
    std::mt19937 random(0x0dda'2026);
    const auto seeds = target.seeds();
    ASSERT_FALSE(seeds.empty());
    for (const auto& seed : seeds) target.run(seed);
    for (int i = 0; i < 3000; ++i) target.run(mutate(seeds[static_cast<size_t>(i) % seeds.size()], random));
    for (int i = 0; i < 500; ++i) {
      net::Bytes noise(std::uniform_int_distribution<size_t>(0, 256)(random));
      for (auto& byte : noise) byte = static_cast<uint8_t>(random());
      target.run(noise);
    }
    target.run({});
  }
}

}  // namespace
}  // namespace dannan::fuzz
