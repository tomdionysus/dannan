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
  config/config.cpp
  encoding/base.cpp
  encoding/percent.cpp
  contract/completion.cpp
  contract/hlc.cpp
  contract/thread_safety.cpp
  identity/argon2.cpp
  identity/certificate.cpp
  identity/cluster_key.cpp
  identity/crypto.cpp
  identity/durable_file.cpp
  identity/key_custody.cpp
  identity/node_id.cpp
  identity/node_identity.cpp
  identity/sealed_counters.cpp
  identity/revocations.cpp
  identity/secure_memory.cpp
  identity/signing.cpp
  membership/node_record.cpp
  membership/identity_reset.cpp
  membership/agreed_roster.cpp
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
  agree/agreement.cpp
  agree/caspaxos.cpp
  agree/memory_store.cpp
  agree/wire.cpp
  listen/certificates.cpp
  listen/connector.cpp
  listen/limits.cpp
  listen/listener.cpp
  listen/stream.cpp
  sched/class_gate.cpp
  sched/cpu_pacing.cpp
  sched/duty_share.cpp
  sched/memory_limit.cpp
  sched/retained_memory.cpp
  sched/work.cpp
  sched/worker_pool.cpp
  status/log.cpp
  status/activity.cpp
  status/events.cpp
  status/observation.cpp
  status/process.cpp
  status/readiness.cpp
  status/surface.cpp
  status/telemetry.cpp
  status/thread_cpu.cpp
  storage/file_system.cpp
  supervise/startup.cpp
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
  PRIVATE dannan_warnings yaml-cpp::yaml-cpp
)
# The SONAME is major.minor: a patch release keeps the ABI (tools/ci.sh abi checks
# it against the minor's last release), so a host built against 0.5.x loads any
# later 0.5 patch. An ABI change is a minor release, with its own SONAME and package.
set_target_properties(dannan PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR} EXPORT_NAME dannan)
add_library(dannan::dannan ALIAS dannan)

# Installed for Dagda, Lugh and Lir: find_package(dannan 0.6.0 CONFIG), the minimum patch, then
# link dannan::dannan. Headers keep their tree, so "dannan/<subsystem>/x.h"
# includes the same installed as in the tree; the test fixtures are shared
# too, the tests and fuzz targets are not.
# The simulation harness's checkers' search, compiled once (the harness design's "Packaging"),
# built and installed whether or not the tests are, so a hosted system's tests link
# dannan::sim_checkers with the installed header-only fixtures (dannan/test/fixtures/sim/), not a
# copy (stack rule 3). Standard library only.
add_library(dannan_sim_checkers STATIC test/fixtures/sim/linearizable.cpp test/fixtures/sim/serializable.cpp test/fixtures/sim/convergence.cpp)
set_target_properties(dannan_sim_checkers PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME sim_checkers)
target_include_directories(dannan_sim_checkers PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dannan_sim_checkers PRIVATE $<BUILD_INTERFACE:dannan_warnings>)
# The same name in a fetched build as installed (Louhi).
add_library(dannan::sim_checkers ALIAS dannan_sim_checkers)

# The stack's one test package (TODO/2026-11-20-stack-plan.md, item 17): the fuzz harness (the
# target registry, the smoke run, replay) with the simulation's checkers, so a hosted system runs
# its own fuzz targets and simulations in its own test binary, linking dannan::testing. Built and
# installed always: it needs no GoogleTest. The fixtures' headers keep their paths
# (dannan/test/fixtures/...), installed with the rest.
add_library(dannan_testing STATIC fuzz/harness.cpp)
set_target_properties(dannan_testing PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME testing)
target_include_directories(dannan_testing PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dannan_testing PUBLIC dannan dannan_sim_checkers PRIVATE $<BUILD_INTERFACE:dannan_warnings>)
add_library(dannan::testing ALIAS dannan_testing)

include(CMakePackageConfigHelpers)
install(TARGETS dannan dannan_sim_checkers dannan_testing EXPORT dannanTargets LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dannan FILES_MATCHING PATTERN "*.h"
        PATTERN "include" EXCLUDE PATTERN "fuzz" EXCLUDE PATTERN "test/plugins" EXCLUDE PATTERN "test/interop" EXCLUDE)
# The fuzz harness's header, without the targets' sources.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/fuzz.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dannan/fuzz)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/dannan/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dannan FILES_MATCHING PATTERN "*.h")
install(FILES ${PROJECT_BINARY_DIR}/generated/dannan/version.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dannan)
install(EXPORT dannanTargets NAMESPACE dannan:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dannan)
configure_package_config_file(${PROJECT_SOURCE_DIR}/cmake/dannanConfig.cmake.in ${PROJECT_BINARY_DIR}/dannanConfig.cmake
                              INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dannan)
# As the SONAME: any later patch of the same minor is compatible.
write_basic_package_version_file(${PROJECT_BINARY_DIR}/dannanConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES ${PROJECT_BINARY_DIR}/dannanConfig.cmake ${PROJECT_BINARY_DIR}/dannanConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dannan)
