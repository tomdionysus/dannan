#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# dannan-version: prints the repository version, as the tag and the CMake
# project version state it.
add_executable(dannan-version version_main.cpp)
target_link_libraries(dannan-version PRIVATE dannan dannan_warnings)

# dannan-node: a node with nothing above it but an echo, for the
# two-process test and the compose test bed.
add_executable(dannan-node dannan_node.cpp)
target_link_libraries(dannan-node PRIVATE dannan dannan_warnings)
target_include_directories(dannan-node PRIVATE "${PROJECT_SOURCE_DIR}")

# dannan-ca: the cluster's certificate authority, on an operator's machine.
add_executable(dannan-ca dannan_ca.cpp)
target_link_libraries(dannan-ca PRIVATE dannan dannan_warnings)
