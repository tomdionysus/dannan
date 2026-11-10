#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# A consumer of the installed package, as Dagda and the Macha port are: it
# must find dannan, and through it its dependencies, on whatever the build
# itself accepted.
cmake_minimum_required(VERSION 3.24)
project(dannan_package_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(dannan ${DANNAN_EXPECTED_VERSION} CONFIG REQUIRED)
add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE dannan::dannan)
