#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

# One number for the repository: both libraries, the tag and --version.
project(dannan VERSION 0.12.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables at the top of the build tree: ./build/dannan_tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(DANNAN_BUILD_TESTING "Build dannan_tests (GoogleTest)" OFF)
option(DANNAN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DANNAN_BUILD_COMPOSE_TESTS "Build dannan_compose_tests, the Docker NAT bed's driver (needs DANNAN_BUILD_TESTING)" OFF)
option(DANNAN_BUILD_FUZZING "Build each fuzz target as its own binary (libFuzzer where the compiler has it)" OFF)
# The API reference and guides, by Doxygen, a build tool only (docs/CMakeLists.txt).
option(DANNAN_BUILD_DOCS "Add the docs target: the API reference and guides, warnings as errors" OFF)
# Off for a project that fetches Dannan for the library alone (Lugh's client library).
option(DANNAN_BUILD_TOOLS "Build and install the tools: dannan-node, dannan-ca, dannan-bench, dannan-import-macha, dannan-version" ON)
if(DANNAN_BUILD_TESTING AND NOT DANNAN_BUILD_TOOLS)
  message(FATAL_ERROR "DANNAN_BUILD_TESTING needs DANNAN_BUILD_TOOLS: the tests run dannan-node and dannan-ca")
endif()

# The stack's one sanitizer switch (docs/building.md): "address" (ASan with UBSan) or "thread"
# (TSan). It instruments every target Dannan builds, fetched or not, and a consumer that sets it
# before fetching Dannan gets an instrumented library: a sanitizer gate never links an
# uninstrumented Dannan (the maintainer, 2026-11-20). Dagda passes it to the Dannan it fetches.
set(DANNAN_SANITIZE "" CACHE STRING "Instrument Dannan: empty, address (ASan and UBSan) or thread (TSan)")
set_property(CACHE DANNAN_SANITIZE PROPERTY STRINGS "" address thread)
if(DANNAN_SANITIZE STREQUAL "address")
  set(DANNAN_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
elseif(DANNAN_SANITIZE STREQUAL "thread")
  set(DANNAN_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT DANNAN_SANITIZE STREQUAL "")
  message(FATAL_ERROR "DANNAN_SANITIZE is empty, address or thread, not \"${DANNAN_SANITIZE}\"")
endif()
if(DANNAN_SANITIZER_FLAGS)
  # This directory and below only: every Dannan target, never the consumer's own.
  add_compile_options(${DANNAN_SANITIZER_FLAGS})
  add_link_options(${DANNAN_SANITIZER_FLAGS})
endif()

# Asio is header-only. The floor is Debian 13's 1.83, which the live nodes run
# and the suite passes on (test/linux, arm64). One number for the build and the
# installed package's config, which a consumer meets (test/linux package).
set(DANNAN_BOOST_MINIMUM 1.83)
find_package(Boost ${DANNAN_BOOST_MINIMUM} CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)
# The configuration file reader's YAML (dannan/config; the maintainer, 2026-11-04).
find_package(yaml-cpp 0.7 CONFIG REQUIRED)
# Where an install puts things; before any target names its install paths.
include(GNUInstallDirs)

add_library(dannan_warnings INTERFACE)
target_compile_options(dannan_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # The lock annotations are checked, not decorative.
  target_compile_options(dannan_warnings INTERFACE -Wthread-safety)
endif()
if(DANNAN_WARNINGS_AS_ERRORS)
  target_compile_options(dannan_warnings INTERFACE -Werror)
endif()

configure_file(dannan/include/dannan/version.h.in ${PROJECT_BINARY_DIR}/generated/dannan/version.h @ONLY)

add_subdirectory(dannan)
if(DANNAN_SANITIZER_FLAGS)
  # What links an instrumented Dannan links the sanitizer's runtime with it. Not installed: a
  # package is never built instrumented.
  target_link_options(dannan INTERFACE $<BUILD_INTERFACE:${DANNAN_SANITIZER_FLAGS}>)
endif()
if(DANNAN_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(DANNAN_BUILD_TESTING OR DANNAN_BUILD_FUZZING)
  add_subdirectory(dannan/fuzz)
endif()

if(DANNAN_BUILD_DOCS)
  add_subdirectory(docs)
endif()

if(DANNAN_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(dannan/test)
  # The examples the guides quote, run by dannan_tests (examples/CMakeLists.txt).
  add_subdirectory(examples)
  target_compile_definitions(dannan_tests PRIVATE DANNAN_EXAMPLES_DIR="${PROJECT_BINARY_DIR}/examples")
  add_dependencies(dannan_tests example_founding_three example_routes example_delivery example_settings example_deadlines example_hosted_node
                   example_simulated_test)
  if(DANNAN_BUILD_COMPOSE_TESTS)
    add_subdirectory(test/compose)
  endif()
endif()
