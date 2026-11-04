//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstdio>
#include <cstring>

#include "dannan/version.h"

int main(int argc, char** argv) {
  if (argc == 1 || (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0))) {
    std::printf("dannan %s\n", dannan::library_version());
    return 0;
  }
  std::fprintf(stderr, "usage: %s [--version]\n", argv[0]);
  return 2;
}
