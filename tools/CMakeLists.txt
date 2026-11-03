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
