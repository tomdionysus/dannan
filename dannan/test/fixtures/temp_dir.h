//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef DANNAN_TEST_TEMP_ROOT
#error "DANNAN_TEST_TEMP_ROOT must name a directory inside the build tree"
#endif

namespace dannan::test {

// A fresh directory for one test, removed with everything in it when the
// object goes. Created under the build tree, never the system temp
// directory: the repository's tests touch nothing outside it.
class TempDir {
 public:
  TempDir() {
    static std::atomic<unsigned> sequence{0};
    _path = std::filesystem::path(DANNAN_TEST_TEMP_ROOT) / (std::to_string(::getpid()) + "-" + std::to_string(sequence.fetch_add(1)));
    std::filesystem::remove_all(_path);
    std::filesystem::create_directories(_path);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(_path, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const noexcept { return _path; }

 private:
  std::filesystem::path _path;
};

}  // namespace dannan::test
