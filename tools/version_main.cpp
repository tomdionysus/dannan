//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <cstdio>
#include <cstring>

#include "dagda/version.h"
#include "dannan/version.h"

int main(int argc, char** argv) {
  if (argc == 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-v") == 0)) {
    std::printf("dannan %s\n", dannan::library_version());
    return 0;
  }
  if (argc == 1) {
    std::printf("dannan %s\ndagda %s\n", dannan::library_version(), dagda::library_version());
    return 0;
  }
  std::fprintf(stderr, "usage: %s [--version]\n", argv[0]);
  return 2;
}
