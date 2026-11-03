//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A fuzz binary's entry: the targets of the one target file it is built
// from, each run on every input. With libFuzzer (DANNAN_LIBFUZZER), the
// fuzzer's own entry point. Without it, a replay driver: each argument is a
// file or a directory of files to run; with no argument, the seeds.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "dannan/fuzz/fuzz.h"

namespace {

void run_all(std::span<const uint8_t> data) {
  for (const auto& target : dannan::fuzz::targets()) target.run(data);
}

}  // namespace

#if defined(DANNAN_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  run_all(std::span<const uint8_t>(data, size));
  return 0;
}
#else
int main(int argc, char** argv) {
  size_t runs = 0;
  const auto run_file = [&](const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const dannan::net::Bytes data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    run_all(data);
    ++runs;
  };
  if (argc == 1) {
    for (const auto& target : dannan::fuzz::targets())
      for (const auto& seed : target.seeds()) {
        run_all(seed);
        ++runs;
      }
  }
  for (int i = 1; i < argc; ++i) {
    const std::filesystem::path path(argv[i]);
    if (std::filesystem::is_directory(path)) {
      for (const auto& entry : std::filesystem::directory_iterator(path))
        if (entry.is_regular_file()) run_file(entry.path());
    } else {
      run_file(path);
    }
  }
  std::printf("%zu inputs through %zu targets, no finding\n", runs, dannan::fuzz::targets().size());
  return 0;
}
#endif
