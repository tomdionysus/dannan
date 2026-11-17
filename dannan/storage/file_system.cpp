//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/storage/file_system.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace dannan::storage {

ErrorClass class_of(int errno_value) noexcept {
  switch (errno_value) {
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
      return ErrorClass::no_room;
    case ENOENT:
    case ENOTDIR:
      return ErrorClass::absent;
    case EIO:
      return ErrorClass::io;
    case EEXIST:
    case ENOTEMPTY:
      return ErrorClass::exists;
    case EACCES:
    case EPERM:
    case EROFS:
      return ErrorClass::denied;
    case EWOULDBLOCK:
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case EBUSY:
      return ErrorClass::busy;
    default:
      return ErrorClass::other;
  }
}

const char* class_name(ErrorClass kind) noexcept {
  switch (kind) {
    case ErrorClass::no_room:
      return "no room";
    case ErrorClass::absent:
      return "absent";
    case ErrorClass::io:
      return "io";
    case ErrorClass::exists:
      return "exists";
    case ErrorClass::denied:
      return "denied";
    case ErrorClass::busy:
      return "busy";
    case ErrorClass::other:
      return "other";
  }
  return "other";
}

std::string describe(const Error& error) {
  return error.operation + " " + error.path.string() + ": " + std::strerror(error.code) + " (" + class_name(error.kind) + ")";
}

namespace {

Error error_of(std::string operation, const Path& path, int code = errno) { return Error{std::move(operation), path, code, class_of(code)}; }

// A failed sync is `io` whatever its errno: what the file holds on disk is unknown.
Error sync_error(std::string operation, const Path& path, int code = errno) { return Error{std::move(operation), path, code, ErrorClass::io}; }

Result<void> sync_fd(int fd, const char* operation, const Path& path) {
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) return sync_error(operation, path);
  return {};
}

class PosixFile final : public File {
 public:
  PosixFile(int fd, Path path) : _fd(fd), _path(std::move(path)) {}
  ~PosixFile() override {
    if (_fd >= 0) ::close(_fd);
  }

  Result<size_t> read_at(uint64_t offset, std::span<uint8_t> into) override {
    size_t done = 0;
    while (done < into.size()) {
      const auto n = ::pread(_fd, into.data() + done, into.size() - done, static_cast<off_t>(offset + done));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) return error_of("read", _path);
      if (n == 0) break;
      done += static_cast<size_t>(n);
    }
    return done;
  }
  Result<size_t> write_at(uint64_t offset, std::span<const uint8_t> bytes) override {
    size_t done = 0;
    while (done < bytes.size()) {
      const auto n = ::pwrite(_fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return error_of("write", _path, n < 0 ? errno : EIO);
      done += static_cast<size_t>(n);
    }
    return done;
  }
  Result<size_t> append(std::span<const uint8_t> bytes) override {
    auto at = size();
    if (!at) return at.error();
    return write_at(at.value(), bytes);
  }
  Result<void> truncate(uint64_t size) override {
    int rc;
    do {
      rc = ::ftruncate(_fd, static_cast<off_t>(size));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) return error_of("truncate", _path);
    return {};
  }
  Result<uint64_t> size() override {
    struct ::stat st{};
    if (::fstat(_fd, &st) != 0) return error_of("stat", _path);
    return static_cast<uint64_t>(st.st_size);
  }
  Result<void> sync() override { return sync_fd(_fd, "sync", _path); }
  Result<void> close() override {
    if (_fd < 0) return {};
    const int fd = _fd;
    _fd = -1;
    if (::close(fd) != 0 && errno != EINTR) return error_of("close", _path);
    return {};
  }

 private:
  int _fd;
  Path _path;
};

class PosixLock final : public Lock {
 public:
  explicit PosixLock(int fd) : _fd(fd) {}
  ~PosixLock() override {
    (void)::flock(_fd, LOCK_UN);
    ::close(_fd);
  }

 private:
  int _fd;
};

EntryType type_of(mode_t mode) {
  if (S_ISREG(mode)) return EntryType::file;
  if (S_ISDIR(mode)) return EntryType::directory;
  return EntryType::other;
}

class PosixFileSystem final : public FileSystem {
 public:
  Result<std::unique_ptr<File>> open(const Path& path, OpenMode mode) override {
    int flags = O_CLOEXEC;
    flags |= mode.write ? (mode.read ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (mode.create) flags |= O_CREAT;
    if (mode.create && mode.exclusive) flags |= O_EXCL;
    if (mode.truncate) flags |= O_TRUNC;
    int fd;
    do {
      fd = ::open(path.c_str(), flags, static_cast<mode_t>(mode.permissions));
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return error_of("open", path);
    return std::unique_ptr<File>(std::make_unique<PosixFile>(fd, path));
  }
  Result<void> rename(const Path& from, const Path& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) return error_of("rename to " + to.string() + " from", from);
    return {};
  }
  Result<void> link(const Path& from, const Path& to) override {
    if (::link(from.c_str(), to.c_str()) != 0) return error_of("link", to);
    return {};
  }
  Result<void> remove(const Path& path) override {
    if (::remove(path.c_str()) != 0) return error_of("remove", path);
    return {};
  }
  Result<std::vector<Entry>> list(const Path& directory) override {
    DIR* dir = ::opendir(directory.c_str());
    if (!dir) return error_of("list", directory);
    std::vector<Entry> out;
    errno = 0;
    while (const auto* entry = ::readdir(dir)) {
      const std::string name = entry->d_name;
      if (name == "." || name == "..") continue;
      struct ::stat st{};
      if (::fstatat(::dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        // Gone between the read and the stat: not listed.
        if (errno == ENOENT) continue;
        const auto failed = error_of("stat", directory / name);
        ::closedir(dir);
        return failed;
      }
      out.push_back(Entry{name, type_of(st.st_mode), static_cast<uint64_t>(st.st_size)});
      errno = 0;
    }
    const int code = errno;
    ::closedir(dir);
    if (code != 0) return error_of("list", directory, code);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return out;
  }
  Result<Entry> stat(const Path& path) override {
    struct ::stat st{};
    if (::stat(path.c_str(), &st) != 0) return error_of("stat", path);
    return Entry{path.filename().string(), type_of(st.st_mode), static_cast<uint64_t>(st.st_size)};
  }
  Result<void> make_directories(const Path& path, unsigned permissions) override {
    Path made;
    for (const auto& part : path) {
      made /= part;
      if (::mkdir(made.c_str(), static_cast<mode_t>(permissions)) == 0) continue;
      if (errno != EEXIST) return error_of("make directory", made);
      // Present: it must be a directory.
      struct ::stat st{};
      if (::stat(made.c_str(), &st) != 0) return error_of("make directory", made);
      if (!S_ISDIR(st.st_mode)) return error_of("make directory", made, EEXIST);
    }
    return {};
  }
  Result<void> sync_directory(const Path& directory) override {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return error_of("open directory", directory);
    auto synced = sync_fd(fd, "sync directory", directory);
    ::close(fd);
    return synced;
  }
  Result<void> sync_all(const Path& any_on_it) override {
#ifdef __linux__
    const int fd = ::open(any_on_it.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return error_of("open", any_on_it);
    const int rc = ::syncfs(fd);
    const int code = errno;
    ::close(fd);
    if (rc != 0) return sync_error("sync all", any_on_it, code);
    return {};
#else
    // No syncfs: the whole system's, which covers this one.
    (void)any_on_it;
    ::sync();
    return {};
#endif
  }
  Result<Space> space(const Path& any_on_it) override {
    struct ::statvfs st{};
    if (::statvfs(any_on_it.c_str(), &st) != 0) return error_of("space", any_on_it);
    return Space{static_cast<uint64_t>(st.f_bavail) * st.f_frsize, static_cast<uint64_t>(st.f_blocks) * st.f_frsize};
  }
  Result<std::unique_ptr<Lock>> lock(const Path& path) override {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return error_of("open lock", path);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const auto failed = error_of("lock", path);
      ::close(fd);
      return failed;
    }
    return std::unique_ptr<Lock>(std::make_unique<PosixLock>(fd));
  }
};

}  // namespace

FileSystem& posix_file_system() {
  static PosixFileSystem the;
  return the;
}

}  // namespace dannan::storage
