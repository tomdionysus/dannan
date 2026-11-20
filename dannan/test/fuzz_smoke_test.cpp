//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Every fuzz target (dannan/fuzz/), on every gate, through the exported harness: its seeds, then
// thousands of mutations of them and random inputs. A target that crashes, or throws what its
// parser does not document, fails here. Deterministic: the same inputs every run, on any
// standard library, so a failure reproduces.

#include <gtest/gtest.h>

#include "dannan/fuzz/fuzz.h"

namespace dannan::fuzz {
namespace {

TEST(Fuzz, EveryTargetSurvivesMutatedAndRandomInputs) {
  const auto& all = targets();
  ASSERT_GE(all.size(), 7u);
  for (const auto& target : all) {
    SCOPED_TRACE(target.name);
    EXPECT_GT(smoke(target), 3500u);
  }
}

}  // namespace
}  // namespace dannan::fuzz
