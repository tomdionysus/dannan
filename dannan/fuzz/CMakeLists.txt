#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Fuzz targets (CONTRIBUTING.md, "Layout"): one per parser of untrusted
# bytes. Every target is compiled into the test binary's smoke test, which
# runs on each gate; with DANNAN_BUILD_FUZZING, each also builds as its own
# binary: a libFuzzer fuzzer where the compiler has libFuzzer, otherwise a
# replay driver over files.
set(DANNAN_FUZZ_TARGETS handshake level0 records)

add_library(dannan_fuzz_targets OBJECT registry.cpp)
foreach(target ${DANNAN_FUZZ_TARGETS})
  target_sources(dannan_fuzz_targets PRIVATE ${target}_fuzz.cpp)
endforeach()
target_link_libraries(dannan_fuzz_targets PUBLIC dannan PRIVATE dannan_warnings)

if(DANNAN_BUILD_FUZZING)
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles("#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) { return 0; }" DANNAN_HAVE_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  foreach(target ${DANNAN_FUZZ_TARGETS})
    add_executable(dannan_fuzz_${target} ${target}_fuzz.cpp registry.cpp driver.cpp)
    target_link_libraries(dannan_fuzz_${target} PRIVATE dannan dannan_warnings)
    if(DANNAN_HAVE_LIBFUZZER)
      target_compile_definitions(dannan_fuzz_${target} PRIVATE DANNAN_LIBFUZZER)
      target_compile_options(dannan_fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
      target_link_options(dannan_fuzz_${target} PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
  endforeach()
endif()
