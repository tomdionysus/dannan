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
target_link_libraries(dannan_test_fixtures INTERFACE GTest::gtest Threads::Threads)
# Tests write only inside the build tree.
target_compile_definitions(dannan_test_fixtures INTERFACE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")

add_executable(dannan_tests
  contract_test.cpp
  fixtures_test.cpp
  identity_crypto_test.cpp
  identity_durable_file_test.cpp
  net_assembler_test.cpp
  net_channel_test.cpp
  net_codec_test.cpp
  net_outbound_test.cpp
  net_routes_record_test.cpp
  net_scheduler_test.cpp
  net_server_test.cpp
  net_session_test.cpp
  sched_memory_test.cpp
  sched_walk_retry_test.cpp
  sched_work_test.cpp
  status_log_test.cpp
  version_test.cpp
)
target_link_libraries(dannan_tests PRIVATE dannan dannan_test_fixtures GTest::gtest_main dannan_warnings)
