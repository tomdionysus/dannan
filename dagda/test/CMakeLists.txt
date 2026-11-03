#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(dagda_tests
  version_test.cpp
)
target_link_libraries(dagda_tests PRIVATE dagda dannan_test_fixtures GTest::gtest_main dannan_warnings)
