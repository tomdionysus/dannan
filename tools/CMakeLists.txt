#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# dannan-version: prints the repository version, as the tag and the CMake
# project version state it.
add_executable(dannan-version version_main.cpp)
target_link_libraries(dannan-version PRIVATE dannan dagda dannan_warnings)

# dannan-node: a node with the transport and nothing above it, for the
# two-process test against a Macha node.
add_executable(dannan-node dannan_node.cpp)
target_link_libraries(dannan-node PRIVATE dannan dannan_warnings)
