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

# dannan-import-macha: Macha's state files into Dannan's formats, once, at the
# first cutover.
add_executable(dannan-import-macha dannan_import_macha.cpp macha_import.cpp)
target_link_libraries(dannan-import-macha PRIVATE dannan dannan_warnings)
target_include_directories(dannan-import-macha PRIVATE "${PROJECT_SOURCE_DIR}")

# dannan-bench: figures to compare, recorded and never asserted. Not in the gate.
add_executable(dannan-bench dannan_bench.cpp)
target_link_libraries(dannan-bench PRIVATE dannan dannan_warnings)
target_include_directories(dannan-bench PRIVATE "${PROJECT_SOURCE_DIR}")

# Installed, as the dannan-tools package ships them: the operator's CA, the
# one-time import and the version. dannan-node and dannan-bench are the test
# bed's and stay in the build tree.
install(TARGETS dannan-ca dannan-import-macha dannan-version RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
