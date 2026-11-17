//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Built against the installed package: a header that reaches Asio and
// OpenSSL, the library's own version at run time, and the simulation
// harness's checkers, which a hosted system's tests link.
#include <cstdio>
#include <cstring>

#include "dannan/net/session.h"
#include "dannan/test/fixtures/sim/linearizable.h"
#include "dannan/test/fixtures/sim/models.h"
#include "dannan/version.h"

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  dannan::test::sim::History history;
  if (!dannan::test::sim::check_linearizable(history, dannan::test::sim::CounterModel{}).linearizable) return 3;
  const char* version = dannan::library_version();
  std::printf("consumer linked dannan %s\n", version);
  return std::strcmp(version, argv[1]) == 0 ? 0 : 1;
}
