//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <filesystem>
#include <string_view>

#include "dannan/storage/file_system.h"

namespace dannan::identity {

// Replaces `path` with `contents` so that a reader, or a node starting after
// a crash at any instant, sees the old contents or the new and never a
// mixture: written to a temporary beside it, synced, renamed over it, and the
// directory synced, with each directory it made synced into its parent. Creates missing
// directories with `directory_mode` (owner-only, 0700, by default; the process's umask applies);
// the file is readable by its owner alone. On failure throws std::runtime_error, leaving the old
// file as it was and no temporary behind. Waits on the state device. Written through `fs` (the
// simulated disk in the harness).
void durable_replace_file(const std::filesystem::path& path, std::string_view contents, storage::FileSystem& fs = storage::posix_file_system(),
                          unsigned directory_mode = 0700);

// Creates `path` with `contents`, never replacing a file already there:
// what durable_replace_file guarantees, but the last step refuses an
// existing name atomically, so of two creators racing exactly one wins. On
// an existing file throws std::filesystem::filesystem_error with
// std::errc::file_exists and leaves that file untouched; otherwise
// std::runtime_error. No temporary is left behind either way. For what is
// made once and never replaced: key custody, a keyring, a table's first
// header. Directories as durable_replace_file's.
void durable_create_file(const std::filesystem::path& path, std::string_view contents, storage::FileSystem& fs = storage::posix_file_system(),
                         unsigned directory_mode = 0700);

// Makes `directory` and each missing directory above it with `mode` (the umask applies), then
// syncs each one made into its parent, deepest first: a directory's entry is durable only once
// its parent is synced (TODO/2026-11-20-stack-plan.md, item 19). Those already there are left as
// they are. Throws std::runtime_error naming the directory if one cannot be made or synced.
// Waits on the state device.
void durable_create_directories(const std::filesystem::path& directory, unsigned mode = 0700, storage::FileSystem& fs = storage::posix_file_system());

// Makes `directory`'s entries durable: what was created, renamed or removed in it. Throws
// std::runtime_error naming it if it cannot be opened or synced. Waits on the state device.
void durable_sync_directory(const std::filesystem::path& directory, storage::FileSystem& fs = storage::posix_file_system());

}  // namespace dannan::identity
