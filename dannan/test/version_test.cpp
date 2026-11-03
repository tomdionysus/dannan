//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/version.h"

#include <gtest/gtest.h>

#include <string>

namespace dannan {
namespace {

// The library a hosted system loads reports the version its headers name;
// the hosted system's refusal of a mismatch rests on this.
TEST(Version, TheLibraryReportsTheVersionItsHeadersName) { EXPECT_STREQ(library_version(), kVersion); }

TEST(Version, IsExactlyMajorMinorPatch) {
  EXPECT_EQ(std::string(kVersion), std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch));
}

}  // namespace
}  // namespace dannan
