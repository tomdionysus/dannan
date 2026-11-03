#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The store: a hosted system on Dannan, never a floor under it. It links
# Dannan; Dannan never includes anything from here.
add_library(dagda SHARED
  version.cpp
)
target_include_directories(dagda
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(dagda
  PUBLIC dannan
  PRIVATE dannan_warnings
)
set_target_properties(dagda PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
