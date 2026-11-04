#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Shared fixtures for both test binaries: the gate, ports and (from step 1)
# the stepped clock. Header-only, included as "dannan/test/fixtures/x.h".
add_library(dannan_test_fixtures INTERFACE)
target_include_directories(dannan_test_fixtures INTERFACE ${PROJECT_SOURCE_DIR})
target_link_libraries(dannan_test_fixtures INTERFACE GTest::gtest Threads::Threads $<$<PLATFORM_ID:Linux>:util>)
# Tests write only inside the build tree.
target_compile_definitions(dannan_test_fixtures INTERFACE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")

add_executable(dannan_tests
  contract_test.cpp
  fixtures_test.cpp
  fuzz_smoke_test.cpp
  identity_crypto_test.cpp
  identity_durable_file_test.cpp
  identity_key_custody_test.cpp
  identity_node_identity_test.cpp
  identity_secure_memory_test.cpp
  identity_signing_test.cpp
  membership_test.cpp
  placement_test.cpp
  reach_paths_test.cpp
  reach_test.cpp
  reach_upnp_test.cpp
  net_assembler_test.cpp
  node_test.cpp
  net_channel_test.cpp
  net_client_test.cpp
  net_codec_test.cpp
  net_fanout_test.cpp
  net_outbound_test.cpp
  net_routes_record_test.cpp
  net_scheduler_test.cpp
  net_server_test.cpp
  net_session_test.cpp
  sched_memory_test.cpp
  sched_walk_retry_test.cpp
  sched_work_test.cpp
  status_log_test.cpp
  status_observation_test.cpp
  status_surface_test.cpp
  status_telemetry_test.cpp
  supervise_test.cpp
  two_process_test.cpp
  version_test.cpp
)
target_link_libraries(dannan_tests PRIVATE dannan dannan_test_fixtures GTest::gtest_main dannan_warnings)
# Every fuzz target, run by the smoke test.
target_sources(dannan_tests PRIVATE $<TARGET_OBJECTS:dannan_fuzz_targets>)

# The supervisor's test plugins, one behaviour each, built as loadable
# modules into one directory the tests point the supervisor at.
set(DANNAN_TEST_PLUGIN_DIR "${PROJECT_BINARY_DIR}/test-plugins")
foreach(kind running writes_on_stop declining faulting faulting_after_start mismatched no_entry)
  add_library(dannan_test_plugin_${kind} MODULE plugins/test_plugin.cpp)
  target_include_directories(dannan_test_plugin_${kind} PRIVATE "${PROJECT_SOURCE_DIR}")
  target_compile_definitions(dannan_test_plugin_${kind} PRIVATE DANNAN_TEST_PLUGIN_KIND="${kind}")
  if(kind STREQUAL "no_entry")
    target_compile_definitions(dannan_test_plugin_${kind} PRIVATE DANNAN_TEST_PLUGIN_NO_ENTRY)
  endif()
  set_target_properties(dannan_test_plugin_${kind} PROPERTIES PREFIX "" OUTPUT_NAME "${kind}" LIBRARY_OUTPUT_DIRECTORY "${DANNAN_TEST_PLUGIN_DIR}")
  target_link_libraries(dannan_test_plugin_${kind} PRIVATE Boost::headers)
  add_dependencies(dannan_tests dannan_test_plugin_${kind})
endforeach()
target_compile_definitions(dannan_tests PRIVATE DANNAN_TEST_PLUGIN_DIR="${DANNAN_TEST_PLUGIN_DIR}")

# The two-process test runs dannan-node as a child.
target_compile_definitions(dannan_tests PRIVATE DANNAN_NODE_BINARY="$<TARGET_FILE:dannan-node>")
add_dependencies(dannan_tests dannan-node)
