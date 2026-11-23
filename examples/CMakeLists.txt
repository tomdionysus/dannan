#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The examples the guides quote (the maintainer's ruling, 2026-11-23): real programs, built with the
# tests and run by them (Examples.EachRunsAndSucceeds in dannan_tests), so they cannot go stale.

set(DANNAN_EXAMPLES founding_three routes delivery settings deadlines hosted_node)
foreach(example ${DANNAN_EXAMPLES})
  add_executable(example_${example} ${example}.cpp)
  target_link_libraries(example_${example} PRIVATE dannan dannan_warnings)
  set_target_properties(example_${example} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/examples" OUTPUT_NAME "${example}")
endforeach()

# The simulated nodes, in a test of its own: a GoogleTest program on the test fixtures.
add_executable(example_simulated_test simulated_test.cpp)
target_link_libraries(example_simulated_test PRIVATE dannan dannan_warnings dannan_test_fixtures GTest::gtest_main)
set_target_properties(example_simulated_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/examples" OUTPUT_NAME "simulated_test")

set(DANNAN_EXAMPLES_DIR "${PROJECT_BINARY_DIR}/examples" PARENT_SCOPE)
