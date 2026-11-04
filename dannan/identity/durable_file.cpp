//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/durable_file.h"

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

namespace dannan::identity {
namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what + ": " + std::strerror(errno)); }

void sync(int fd, const std::string& what) {
  int rc;
  do {
    rc = ::fsync(fd);
  } while (rc != 0 && errno == EINTR);
  if (rc != 0) fail(what);
}

void write_all(int fd, std::string_view contents) {
  size_t done = 0;
  while (done < contents.size()) {
    const auto n = ::write(fd, contents.data() + done, contents.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) fail("durable state write failed");
    done += static_cast<size_t>(n);
  }
}

}  // namespace

namespace {

// A temporary beside `path`, written and synced; its name. Unique per
// process and call, so two writers never share one.
std::string written_temporary(const std::filesystem::path& path, std::string_view contents) {
  static std::atomic<uint64_t> sequence{0};
  const auto temp = path.string() + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(sequence.fetch_add(1));
  int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) fail("cannot create durable state " + temp);
  try {
    write_all(fd, contents);
    sync(fd, "cannot sync durable state " + temp);
    const int closing = fd;
    fd = -1;
    if (::close(closing) != 0) fail("cannot close durable state " + temp);
  } catch (...) {
    if (fd >= 0) ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    throw;
  }
  return temp;
}

void sync_directory(const std::filesystem::path& directory) {
  const int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) fail("cannot open durable state directory " + directory.string());
  try {
    sync(dir, "cannot sync durable state directory " + directory.string());
  } catch (...) {
    ::close(dir);
    throw;
  }
  ::close(dir);
}

std::filesystem::path directory_of(const std::filesystem::path& path) {
  const auto directory = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
  std::filesystem::create_directories(directory);
  return directory;
}

}  // namespace

void durable_replace_file(const std::filesystem::path& path, std::string_view contents) {
  const auto directory = directory_of(path);
  const auto temp = written_temporary(path, contents);
  try {
    if (::rename(temp.c_str(), path.c_str()) != 0) fail("cannot replace durable state " + path.string());
    sync_directory(directory);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    throw;
  }
}

void durable_create_file(const std::filesystem::path& path, std::string_view contents) {
  const auto directory = directory_of(path);
  const auto temp = written_temporary(path, contents);
  // link() refuses an existing name atomically, where a check followed by a
  // rename could not: of two creators, exactly one wins.
  if (::link(temp.c_str(), path.c_str()) != 0) {
    const auto error = errno;
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    if (error == EEXIST) throw std::filesystem::filesystem_error("durable state already exists", path, std::make_error_code(std::errc::file_exists));
    errno = error;
    fail("cannot create durable state " + path.string());
  }
  std::error_code ignored;
  std::filesystem::remove(temp, ignored);
  sync_directory(directory);
}

}  // namespace dannan::identity
