//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// A fuzz binary's entry: the targets of the one target file it is built from, each run on every
// input. With libFuzzer (DANNAN_LIBFUZZER), the fuzzer's own entry point. Without it, a replay
// driver: each argument is a file or a directory of files to run; with no argument, the seeds.
// The two lines a hosted system's own fuzz binary needs (fuzz.h).

#include <cstdio>

#include "dannan/fuzz/fuzz.h"

#if defined(DANNAN_LIBFUZZER)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  dannan::fuzz::run_all(dannan::fuzz::targets(), std::span<const uint8_t>(data, size));
  return 0;
}
#else
int main(int argc, char** argv) {
  const auto runs = dannan::fuzz::replay(dannan::fuzz::targets(), argc, argv);
  std::printf("%zu inputs through %zu targets, no finding\n", runs, dannan::fuzz::targets().size());
  return 0;
}
#endif
