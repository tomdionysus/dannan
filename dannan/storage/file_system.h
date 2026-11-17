//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// One file-system interface for the stack (TODO/2026-11-15-file-system-interface.md, Dagda's
// review taken): every path to the disk goes through it, so a simulated disk can sit under all of
// them. Nothing here throws for an I/O failure; a fault is a value the caller decides on, with
// one throwing helper, or_throw(), for code that throws today.
namespace dannan::storage {

using Path = std::filesystem::path;

// What the errno means to a caller, which acts on each differently (Dagda's review).
enum class ErrorClass : uint8_t {
  // ENOSPC, EDQUOT: a refusal; try the next backend.
  no_room,
  // ENOENT, ENOTDIR: not a failure on open or read.
  absent,
  // EIO, and every failed sync: what the file holds on disk is unknown.
  io,
  // EEXIST, ENOTEMPTY.
  exists,
  // EACCES, EPERM, EROFS.
  denied,
  // EWOULDBLOCK, EBUSY: a lock another holds.
  busy,
  other,
};

struct Error {
  // What was being done ("open", "sync", ...), and to what.
  std::string operation;
  Path path;
  int code = 0;
  ErrorClass kind = ErrorClass::other;
};

ErrorClass class_of(int errno_value) noexcept;
const char* class_name(ErrorClass) noexcept;
// "<operation> <path>: <strerror> (<class>)".
std::string describe(const Error& error);

// What or_throw() raises: the error, kept.
class StorageError : public std::runtime_error {
 public:
  explicit StorageError(Error error) : std::runtime_error(describe(error)), _error(std::move(error)) {}
  const Error& error() const noexcept { return _error; }

 private:
  Error _error;
};

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : _held(std::move(value)) {}
  Result(Error error) : _held(std::move(error)) {}
  bool ok() const noexcept { return _held.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }
  T& value() & { return std::get<0>(_held); }
  T&& value() && { return std::get<0>(std::move(_held)); }
  const T& value() const& { return std::get<0>(_held); }
  const Error& error() const { return std::get<1>(_held); }
  T or_throw() && {
    if (!ok()) throw StorageError(std::get<1>(_held));
    return std::get<0>(std::move(_held));
  }

 private:
  std::variant<T, Error> _held;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : _error(std::move(error)) {}
  bool ok() const noexcept { return !_error; }
  explicit operator bool() const noexcept { return ok(); }
  const Error& error() const { return *_error; }
  void or_throw() const {
    if (_error) throw StorageError(*_error);
  }

 private:
  std::optional<Error> _error;
};

struct OpenMode {
  bool read = true;
  bool write = false;
  // Made if absent; `exclusive`: refused (exists) if present.
  bool create = false;
  bool exclusive = false;
  bool truncate = false;
  // For a file created.
  unsigned permissions = 0600;

  static OpenMode reading() { return {}; }
  static OpenMode writing() { return {true, true, false, false, false, 0600}; }
  static OpenMode creating(bool exclusive = false) { return {true, true, true, exclusive, false, 0600}; }
};

enum class EntryType : uint8_t { file, directory, other };

struct Entry {
  std::string name;
  EntryType type = EntryType::file;
  uint64_t size = 0;
};

// statvfs's view, in bytes.
struct Space {
  uint64_t available = 0;
  uint64_t total = 0;
};

// An open file. Not thread-safe: one user at a time.
class File {
 public:
  virtual ~File() = default;
  // Bytes read, fewer at the end of the file.
  virtual Result<size_t> read_at(uint64_t offset, std::span<uint8_t> into) = 0;
  // Writes all of `bytes` at `offset`, or fails.
  virtual Result<size_t> write_at(uint64_t offset, std::span<const uint8_t> bytes) = 0;
  // Writes all of `bytes` at the end. Not atomic against another writer of the same file.
  virtual Result<size_t> append(std::span<const uint8_t> bytes) = 0;
  virtual Result<void> truncate(uint64_t size) = 0;
  virtual Result<uint64_t> size() = 0;
  // Makes this file's data durable (fsync). The sync contract: success means the device said
  // so, which it may not mean (a lying sync); a failure is `io`, and what the file holds on disk
  // is then unknown, so a caller treats it as suspect (Linux may drop the dirty pages and report
  // later syncs as good). A caller survives a lying sync and a torn write by checksums or seals on
  // what it reads back, and relies on no atomicity beyond rename's.
  virtual Result<void> sync() = 0;
  // Closes it; a failure is reported (some file systems report a write's failure only here).
  // Closed by the destructor if not before, the failure then unseen.
  virtual Result<void> close() = 0;
};

// An advisory lock (flock), held for the handle's life.
class Lock {
 public:
  virtual ~Lock() = default;
};

// A file system as the stack writes it. Every operation names its path and returns what
// happened. Thread-safe.
class FileSystem {
 public:
  virtual ~FileSystem() = default;
  virtual Result<std::unique_ptr<File>> open(const Path& path, OpenMode mode) = 0;
  // Replaces `to` with `from` atomically.
  virtual Result<void> rename(const Path& from, const Path& to) = 0;
  // A second name for `from`, refused (exists) if `to` is taken: of two creators, exactly one wins.
  virtual Result<void> link(const Path& from, const Path& to) = 0;
  // A file, or an empty directory.
  virtual Result<void> remove(const Path& path) = 0;
  // A directory's entries, each with its type and size (one stat each, Dagda's review), sorted
  // by name.
  virtual Result<std::vector<Entry>> list(const Path& directory) = 0;
  virtual Result<Entry> stat(const Path& path) = 0;
  // Each missing directory made with `permissions`; those present are left as they are.
  virtual Result<void> make_directories(const Path& path, unsigned permissions = 0700) = 0;
  // Makes a directory's entries (creates, renames, removes) durable.
  virtual Result<void> sync_directory(const Path& directory) = 0;
  // Makes everything written on the file system holding `any_on_it` durable (syncfs).
  virtual Result<void> sync_all(const Path& any_on_it) = 0;
  virtual Result<Space> space(const Path& any_on_it) = 0;
  // Taken at once or refused (busy); made if absent. Held by one handle, of any process.
  virtual Result<std::unique_ptr<Lock>> lock(const Path& path) = 0;
};

// The device: POSIX, as the stack writes today.
FileSystem& posix_file_system();

}  // namespace dannan::storage
