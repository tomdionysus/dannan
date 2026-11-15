//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Node-local rollback's first layer (the maintainer, 2026-11-14): counters that only go up,
// sealed beside custody, durable when advanced, lazily when noted (Dagda's review).

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

#include "dannan/identity/sealed_counters.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::identity {
namespace {

Secret key_of(uint8_t fill) { return Secret(std::vector<uint8_t>(32, fill)); }

// A value read back after the counters are opened again: what a restart sees.
uint64_t reopened(const std::filesystem::path& state, std::string_view label, uint8_t fill = 1) { return SealedCounters(state, key_of(fill)).read(label); }

TEST(SealedCounters, AnAdvanceIsDurableAndOnlyGoesUp) {
  test::TempDir dir;
  SealedCounters counters(dir.path(), key_of(1));
  EXPECT_EQ(counters.read("dagda/heads/01/1"), 0u);
  counters.advance("dagda/heads/01/1", 5);
  EXPECT_EQ(counters.read("dagda/heads/01/1"), 5u);
  EXPECT_EQ(reopened(dir.path(), "dagda/heads/01/1"), 5u);
  // Equal is nothing to do; lower is refused, naming the label and never a value.
  counters.advance("dagda/heads/01/1", 5);
  try {
    counters.advance("dagda/heads/01/1", 4);
    ADD_FAILURE() << "a counter went down";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("dagda/heads/01/1"), std::string::npos) << error.what();
    EXPECT_EQ(std::string(error.what()).find('5'), std::string::npos) << error.what();
  }
  EXPECT_EQ(counters.read("dagda/heads/01/1"), 5u);
  EXPECT_THROW(counters.advance("", 1), std::invalid_argument);
  EXPECT_EQ(counters.path(), dir.path() / "identity" / "counters");
}

// Several in one durable write, checked all before any is taken.
TEST(SealedCounters, ABatchIsOneWriteAndAllOrNone) {
  test::TempDir dir;
  SealedCounters counters(dir.path(), key_of(1));
  counters.advance(std::map<std::string, uint64_t>{{"a", 1}, {"b", 2}});
  EXPECT_EQ(reopened(dir.path(), "a"), 1u);
  EXPECT_EQ(reopened(dir.path(), "b"), 2u);
  EXPECT_THROW(counters.advance(std::map<std::string, uint64_t>{{"a", 9}, {"b", 1}}), std::invalid_argument);
  EXPECT_EQ(counters.read("a"), 1u);
  EXPECT_EQ(reopened(dir.path(), "a"), 1u);
}

// A note is read at once and made durable at the next flush (Dagda: a counter one
// commit behind refuses nothing it should accept).
TEST(SealedCounters, ANoteIsDurableAtTheNextFlush) {
  test::TempDir dir;
  SealedCounters counters(dir.path(), key_of(1));
  EXPECT_FALSE(counters.flush());
  counters.advance("space", 3);
  counters.note("space", 7);
  EXPECT_EQ(counters.read("space"), 7u);
  EXPECT_EQ(reopened(dir.path(), "space"), 3u);
  EXPECT_THROW(counters.note("space", 6), std::invalid_argument);
  EXPECT_TRUE(counters.flush());
  EXPECT_EQ(reopened(dir.path(), "space"), 7u);
  EXPECT_FALSE(counters.flush());
}

// The file opens only under its key, and an altered file is refused, naming the path.
TEST(SealedCounters, OpensOnlyUnderItsKeyAndRefusesAnAlteredFile) {
  test::TempDir dir;
  {
    SealedCounters counters(dir.path(), key_of(1));
    counters.advance("x", 1);
  }
  try {
    SealedCounters other(dir.path(), key_of(2));
    ADD_FAILURE() << "opened under another key";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find((dir.path() / "identity" / "counters").string()), std::string::npos) << error.what();
  }
  {
    std::fstream file(dir.path() / "identity" / "counters", std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(-1, std::ios::end);
    file.put('\x5a');
  }
  EXPECT_THROW(SealedCounters(dir.path(), key_of(1)), std::runtime_error);
}

}  // namespace
}  // namespace dannan::identity
