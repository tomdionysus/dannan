#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The network layer. Public headers are dannan/include/dannan/*.h, included
# as "dannan/x.h"; everything else is included from the repository root as
# "dannan/<subsystem>/x.h".
add_library(dannan SHARED
  contract/thread_safety.cpp
  identity/cluster_key.cpp
  identity/crypto.cpp
  identity/durable_file.cpp
  identity/node_id.cpp
  membership/node_record.cpp
  net/channel.cpp
  net/codec.cpp
  net/routes.cpp
  sched/memory_limit.cpp
  sched/retained_memory.cpp
  sched/work.cpp
  status/log.cpp
  version.cpp
)
target_include_directories(dannan
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(dannan
  PUBLIC Boost::headers OpenSSL::Crypto Threads::Threads
  PRIVATE dannan_warnings
)
set_target_properties(dannan PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
