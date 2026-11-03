#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

# One number for the repository: both libraries, the tag and --version.
project(dannan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables at the top of the build tree: ./build/dannan_tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(DANNAN_BUILD_TESTING "Build dannan_tests and dagda_tests (GoogleTest)" OFF)
option(DANNAN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Asio is header-only; 1.87 is AthenaSIP's floor and the two must agree.
find_package(Boost 1.87 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

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
add_subdirectory(dagda)
add_subdirectory(tools)

if(DANNAN_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(dannan/test)
  add_subdirectory(dagda/test)
endif()
