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
  identity/argon2.cpp
  identity/certificate.cpp
  identity/cluster_key.cpp
  identity/crypto.cpp
  identity/durable_file.cpp
  identity/key_custody.cpp
  identity/node_id.cpp
  identity/node_identity.cpp
  identity/revocations.cpp
  identity/secure_memory.cpp
  identity/signing.cpp
  membership/node_record.cpp
  membership/identity_reset.cpp
  membership/membership.cpp
  net/assembler.cpp
  net/channel.cpp
  net/client.cpp
  net/codec.cpp
  net/fanout.cpp
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
  reach/paths.cpp
  reach/upnp.cpp
  sched/duty_share.cpp
  sched/memory_limit.cpp
  sched/retained_memory.cpp
  sched/work.cpp
  status/log.cpp
  status/activity.cpp
  status/events.cpp
  status/observation.cpp
  status/process.cpp
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
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(dannan
  PUBLIC Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE dannan_warnings
)
set_target_properties(dannan PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} EXPORT_NAME dannan)
add_library(dannan::dannan ALIAS dannan)

# Installed for Dagda, Lugh and Lir: find_package(dannan 0.2 CONFIG), then
# link dannan::dannan. Headers keep their tree, so "dannan/<subsystem>/x.h"
# includes the same installed as in the tree; the test fixtures are shared
# too, the tests and fuzz targets are not.
include(CMakePackageConfigHelpers)
install(TARGETS dannan EXPORT dannanTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dannan FILES_MATCHING PATTERN "*.h"
        PATTERN "include" EXCLUDE PATTERN "fuzz" EXCLUDE PATTERN "test/plugins" EXCLUDE PATTERN "test/interop" EXCLUDE)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/dannan/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dannan FILES_MATCHING PATTERN "*.h")
install(FILES ${PROJECT_BINARY_DIR}/generated/dannan/version.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dannan)
install(EXPORT dannanTargets NAMESPACE dannan:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dannan)
configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/dannanConfig.cmake.in ${PROJECT_BINARY_DIR}/dannanConfig.cmake
                              INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dannan)
# 0.x: a minor version is a new API, so only the same minor is compatible.
write_basic_package_version_file(${PROJECT_BINARY_DIR}/dannanConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${PROJECT_BINARY_DIR}/dannanConfig.cmake ${PROJECT_BINARY_DIR}/dannanConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dannan)
