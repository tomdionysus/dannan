//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dagda/version.h"

#include <gtest/gtest.h>

#include "dannan/version.h"

namespace dagda {
namespace {

// One number for the repository: Dagda is versioned with Dannan.
TEST(Version, DagdaIsVersionedWithDannan) { EXPECT_STREQ(library_version(), dannan::library_version()); }

}  // namespace
}  // namespace dagda
