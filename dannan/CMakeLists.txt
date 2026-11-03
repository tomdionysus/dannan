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
  identity/node_identity.cpp
  membership/node_record.cpp
  membership/identity_reset.cpp
  membership/membership.cpp
  net/assembler.cpp
  net/channel.cpp
  net/client.cpp
  net/codec.cpp
  net/message.cpp
  net/outbound.cpp
  net/routes.cpp
  net/scheduler.cpp
  net/server.cpp
  net/session.cpp
  placement/placement.cpp
  reach/connectivity.cpp
  reach/dial_back.cpp
  reach/external_ip.cpp
  reach/inbound.cpp
  reach/upnp.cpp
  sched/memory_limit.cpp
  sched/retained_memory.cpp
  sched/work.cpp
  status/log.cpp
  status/activity.cpp
  status/events.cpp
  status/observation.cpp
  status/readiness.cpp
  status/surface.cpp
  status/telemetry.cpp
  supervise/supervised.cpp
  supervise/supervisor.cpp
  version.cpp
  node.cpp
)
target_include_directories(dannan
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(dannan
  PUBLIC Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE dannan_warnings
)
set_target_properties(dannan PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
