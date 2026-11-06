//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-import-macha [--dry-run] <state>: Macha's roster, inbound answer and
// last-known telemetry, into Dannan's formats, once, with the node stopped. Each
// original stays as <name>.macha; restoring those is the rollback.

#include <iostream>
#include <string>

#include "tools/macha_import.h"

int main(int argc, char** argv) {
  dannan::tools::ImportOptions options;
  std::string state;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--dry-run")
      options.dry_run = true;
    else if (state.empty() && !argument.starts_with("-"))
      state = argument;
    else
      state.clear(), argc = 0;
  }
  if (state.empty()) {
    std::cerr << "usage: dannan-import-macha [--dry-run] <state directory>\n";
    return 2;
  }
  try {
    const auto report = dannan::tools::import_macha_state(state, options);
    for (const auto& file : report.files) {
      std::cout << file.path << ": " << file.outcome;
      if (!file.detail.empty()) std::cout << " (" << file.detail << ")";
      std::cout << "\n";
    }
    if (options.dry_run) std::cout << "dry run: nothing changed\n";
    return 0;
  } catch (const dannan::tools::ImportRefused& refused) {
    std::cerr << "refused: " << refused.what() << "\n";
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "failed: " << error.what() << "\n";
    return 1;
  }
}
