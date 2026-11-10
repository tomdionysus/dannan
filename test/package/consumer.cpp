//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Built against the installed package: a header that reaches Asio and
// OpenSSL, and the library's own version at run time.
#include <cstdio>
#include <cstring>

#include "dannan/net/session.h"
#include "dannan/version.h"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const char* version = dannan::library_version();
  std::printf("consumer linked dannan %s\n", version);
  return std::strcmp(version, argv[1]) == 0 ? 0 : 1;
}
