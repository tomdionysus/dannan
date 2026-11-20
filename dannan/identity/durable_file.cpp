//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/durable_file.h"

#include <unistd.h>

#include <atomic>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace dannan::identity {
namespace {

[[noreturn]] void fail(const std::string& what, const storage::Error& error) { throw std::runtime_error(what + ": " + std::strerror(error.code)); }

template <class T>
T must(storage::Result<T> result, const std::string& what) {
  if (!result) fail(what, result.error());
  return std::move(result).value();
}
void must(const storage::Result<void>& result, const std::string& what) {
  if (!result) fail(what, result.error());
}

// A temporary beside `path`, written and synced; its name. Unique per process and call, so two
// writers never share one.
storage::Path written_temporary(storage::FileSystem& fs, const storage::Path& path, std::string_view contents) {
  static std::atomic<uint64_t> sequence{0};
  const storage::Path temp = path.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(sequence.fetch_add(1));
  auto file = must(fs.open(temp, storage::OpenMode::creating(true)), "cannot create durable state " + temp.string());
  try {
    const auto bytes = std::span(reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
    must(file->write_at(0, bytes), "durable state write failed");
    must(file->sync(), "cannot sync durable state " + temp.string());
    must(file->close(), "cannot close durable state " + temp.string());
  } catch (...) {
    file.reset();
    (void)fs.remove(temp);
    throw;
  }
  return temp;
}

void sync_directory(storage::FileSystem& fs, const storage::Path& directory) {
  const auto synced = fs.sync_directory(directory);
  if (synced) return;
  fail((synced.error().operation == "open directory" ? "cannot open durable state directory " : "cannot sync durable state directory ") + directory.string(),
       synced.error());
}

// The file's directory, and those this call made for it, deepest first.
struct Directories {
  storage::Path directory;
  std::vector<storage::Path> made;
};

// Makes `directory` and each missing one above it with `mode`; existing ones are left as they are.
Directories made_directories(storage::FileSystem& fs, const storage::Path& directory, unsigned mode) {
  Directories out{directory.empty() ? storage::Path(".") : directory, {}};
  for (auto at = out.directory; !at.empty() && at != at.root_path(); at = at.parent_path()) {
    if (fs.stat(at)) break;
    out.made.push_back(at);
  }
  const auto made = fs.make_directories(out.directory, mode);
  if (!made) fail("cannot create durable state directory " + made.error().path.string(), made.error());
  return out;
}

// The file's directory, made as made_directories() makes it.
Directories directory_of(storage::FileSystem& fs, const storage::Path& path, unsigned mode) { return made_directories(fs, path.parent_path(), mode); }

// Each directory made synced into its parent, deepest first.
void sync_made(storage::FileSystem& fs, const Directories& directories) {
  for (const auto& made : directories.made) sync_directory(fs, made.parent_path().empty() ? storage::Path(".") : made.parent_path());
}

// The file's directory synced, then each directory made for it into its parent, deepest first:
// a directory's entry is durable only once its parent is synced, so a crash could otherwise lose
// a new directory and the file in it.
void sync_directories(storage::FileSystem& fs, const Directories& directories) {
  sync_directory(fs, directories.directory);
  sync_made(fs, directories);
}

}  // namespace

void durable_replace_file(const std::filesystem::path& path, std::string_view contents, storage::FileSystem& fs, unsigned directory_mode) {
  const auto directories = directory_of(fs, path, directory_mode);
  const auto temp = written_temporary(fs, path, contents);
  try {
    must(fs.rename(temp, path), "cannot replace durable state " + path.string());
    sync_directories(fs, directories);
  } catch (...) {
    (void)fs.remove(temp);
    throw;
  }
}

void durable_create_file(const std::filesystem::path& path, std::string_view contents, storage::FileSystem& fs, unsigned directory_mode) {
  const auto directories = directory_of(fs, path, directory_mode);
  const auto temp = written_temporary(fs, path, contents);
  // link() refuses an existing name atomically, where a check followed by a rename could not:
  // of two creators, exactly one wins.
  if (const auto linked = fs.link(temp, path); !linked) {
    (void)fs.remove(temp);
    if (linked.error().kind == storage::ErrorClass::exists)
      throw std::filesystem::filesystem_error("durable state already exists", path, std::make_error_code(std::errc::file_exists));
    fail("cannot create durable state " + path.string(), linked.error());
  }
  (void)fs.remove(temp);
  sync_directories(fs, directories);
}

void durable_create_directories(const std::filesystem::path& directory, unsigned mode, storage::FileSystem& fs) {
  sync_made(fs, made_directories(fs, directory, mode));
}

void durable_sync_directory(const std::filesystem::path& directory, storage::FileSystem& fs) { sync_directory(fs, directory); }

}  // namespace dannan::identity
