//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

// One Macha state directory into Dannan's formats, once, at the first cutover:
// the roster, the inbound answer and the last-known telemetry. Macha's bytes live
// here and nowhere else in Dannan, whose own loaders keep refusing what is not
// theirs. TODO/2026-11-05-first-cutover.md, section 4, has the layouts.
namespace dannan::tools {

// The state cannot be imported, for the reason given. Nothing has been changed.
class ImportRefused : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ImportOptions {
  bool dry_run = false;
  // The wall time a roster older than MACHMEM4 is taken to have last been heard.
  std::function<uint64_t()> now_unix_ms;
};

struct ImportedFile {
  std::string path;  // relative to the state directory
  // imported, already (Dannan's format), absent, or ignored (a bad file only an
  // observer reads, which a node ignores too: left as it was).
  std::string outcome;
  std::string detail;
};

struct ImportReport {
  std::vector<ImportedFile> files;
};

// Reads everything before changing anything. Each original is kept beside its
// replacement as `<name>.macha`, which is what a rollback to Macha restores, and a
// second run changes nothing. Throws ImportRefused for a running node (either
// lock held), no node id, or a roster that cannot be trusted.
ImportReport import_macha_state(const std::filesystem::path& state, const ImportOptions& options = {});

}  // namespace dannan::tools
