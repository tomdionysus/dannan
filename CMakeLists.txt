#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

# One number for the repository: both libraries, the tag and --version.
project(dannan VERSION 0.8.0 LANGUAGES CXX)

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
# Off for a project that fetches Dannan for the library alone (Lugh's client library).
option(DANNAN_BUILD_TOOLS "Build and install the tools: dannan-node, dannan-ca, dannan-bench, dannan-import-macha, dannan-version" ON)
if(DANNAN_BUILD_TESTING AND NOT DANNAN_BUILD_TOOLS)
  message(FATAL_ERROR "DANNAN_BUILD_TESTING needs DANNAN_BUILD_TOOLS: the tests run dannan-node and dannan-ca")
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
if(DANNAN_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(DANNAN_BUILD_TESTING OR DANNAN_BUILD_FUZZING)
  add_subdirectory(dannan/fuzz)
endif()

if(DANNAN_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(dannan/test)
  if(DANNAN_BUILD_COMPOSE_TESTS)
    add_subdirectory(test/compose)
  endif()
endif()
