#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The compose test bed's driver: GoogleTest, its own binary, built only with
# DANNAN_BUILD_COMPOSE_TESTS and run by hand with Docker running; never in
# the per-commit gate (Tom, 2026-11-04).
find_package(GTest 1.14 CONFIG REQUIRED)
add_executable(dannan_compose_tests compose_test.cpp)
target_link_libraries(dannan_compose_tests PRIVATE dannan_test_fixtures GTest::gtest_main dannan_warnings)
# The host's own dannan-node and dannan-ca make the certificate bed's
# identities before it comes up, as an operator would.
add_dependencies(dannan_compose_tests dannan-node dannan-ca)
target_compile_definitions(dannan_compose_tests PRIVATE DANNAN_COMPOSE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" DANNAN_NODE_BINARY="$<TARGET_FILE:dannan-node>"
                                                        DANNAN_CA_BINARY="$<TARGET_FILE:dannan-ca>")
