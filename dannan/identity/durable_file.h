//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <string_view>

namespace dannan::identity {

// Replaces `path` with `contents` so that a reader, or a node starting after
// a crash at any instant, sees the old contents or the new and never a
// mixture: written to a temporary beside it, synced, renamed over it, and the
// directory synced. Creates missing directories owner-only (0700); the file
// is readable by its owner alone. On failure throws std::runtime_error, leaving the old file as
// it was and no temporary behind. Waits on the state device.
void durable_replace_file(const std::filesystem::path& path, std::string_view contents);

// Creates `path` with `contents`, never replacing a file already there:
// what durable_replace_file guarantees, but the last step refuses an
// existing name atomically, so of two creators racing exactly one wins. On
// an existing file throws std::filesystem::filesystem_error with
// std::errc::file_exists and leaves that file untouched; otherwise
// std::runtime_error. No temporary is left behind either way. For what is
// made once and never replaced: key custody, a keyring, a table's first
// header.
void durable_create_file(const std::filesystem::path& path, std::string_view contents);

}  // namespace dannan::identity
