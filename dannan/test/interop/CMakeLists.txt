#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# dannan_interop_tests: Dannan against Macha's own code, in one process, so
# "protocol 23 byte for byte" is checked rather than claimed. Built only
# when DANNAN_MACHA_INTEROP_DIR names a Macha source tree with its core
# library built in <dir>/build (a pinned export, never a live checkout):
#
#   git -C ../macha archive 0.91.2 | tar -x -C build/interop/macha-0.91.2
#   (configure and build its macha_core target in build/interop/macha-0.91.2/build)
#   cmake -S . -B build -DDANNAN_MACHA_INTEROP_DIR=$PWD/build/interop/macha-0.91.2
#
# With DANNAN_MACHA_BINARY naming a Macha node binary, it also holds the
# two-process test: that node and dannan-node over loopback.

set(macha "${DANNAN_MACHA_INTEROP_DIR}")
if(NOT EXISTS "${macha}/build/libmacha_core.dylib" AND NOT EXISTS "${macha}/build/libmacha_core.so")
  message(FATAL_ERROR "DANNAN_MACHA_INTEROP_DIR: no macha_core built under ${macha}/build")
endif()
find_library(MACHA_CORE macha_core PATHS "${macha}/build" NO_DEFAULT_PATH REQUIRED)

add_executable(dannan_interop_tests interop_test.cpp)
target_include_directories(dannan_interop_tests SYSTEM PRIVATE "${macha}/src" "${macha}/build/generated")
target_link_libraries(dannan_interop_tests PRIVATE dannan dannan_test_fixtures GTest::gtest_main ${MACHA_CORE} OpenSSL::Crypto)
set_target_properties(dannan_interop_tests PROPERTIES BUILD_RPATH "${macha}/build")

set(DANNAN_MACHA_BINARY "" CACHE FILEPATH "A Macha node binary for the two-process test")
if(DANNAN_MACHA_BINARY)
  target_sources(dannan_interop_tests PRIVATE two_process_test.cpp)
  target_compile_definitions(dannan_interop_tests PRIVATE DANNAN_MACHA_BINARY="${DANNAN_MACHA_BINARY}" DANNAN_NODE_BINARY="$<TARGET_FILE:dannan-node>")
  add_dependencies(dannan_interop_tests dannan-node)
  target_include_directories(dannan_interop_tests PRIVATE "${PROJECT_SOURCE_DIR}")
endif()
