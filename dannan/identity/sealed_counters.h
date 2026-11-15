//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/secure_memory.h"

namespace dannan::identity {

// Counters that only go up, beside custody (the maintainer, 2026-11-14: node-local rollback,
// the first layer): for a hosted system's node-local state to refuse a copy of
// itself restored from before what it has seen. Every label's value is in one file,
// `<state>/identity/counters`, sealed with AES-256-GCM under the key it is given
// (custody's derive("dannan/counter/1")) and replaced durably on each write.
//
// It refuses one directory restored alone, not a whole state restored together:
// the counters come back with it. The peers' witness is the second layer (0.9).
//
// An advance is durable before it returns. A note is the same promise made lazily
// (Dagda: a space advances on every commit, and a counter one commit behind refuses
// nothing it should accept): durable at the next flush(), which the node makes at
// least once a second and at stop. read() includes what was noted. Thread-safe;
// writes wait on the state device, one at a time.
class SealedCounters {
 public:
  // Opens the file, or begins an empty one. Throws std::runtime_error for a file
  // that does not open under `key` (another key, or altered), naming the path.
  SealedCounters(std::filesystem::path state, Secret key);
  SealedCounters(const SealedCounters&) = delete;
  SealedCounters& operator=(const SealedCounters&) = delete;

  // Zero for a label never advanced.
  uint64_t read(std::string_view label) const;
  // Raises `label` to `value`, durably. Equal is nothing to do; lower throws
  // std::invalid_argument naming the label, never a value. An empty label is refused.
  void advance(std::string_view label, uint64_t value);
  // Several at once, in one durable write; checked all before any is taken.
  void advance(const std::map<std::string, uint64_t>& values);
  // As advance, durable at the next flush rather than now.
  void note(std::string_view label, uint64_t value);
  // Writes what was noted; false when there was nothing to write.
  bool flush();

  const std::filesystem::path& path() const noexcept { return _path; }

 private:
  void check(std::string_view label, uint64_t value) const DANNAN_REQUIRES(_mutex);
  void write() DANNAN_REQUIRES(_mutex);

  const std::filesystem::path _path;
  const Secret _key;
  mutable contract::Mutex _mutex;
  std::map<std::string, uint64_t, std::less<>> _values DANNAN_GUARDED_BY(_mutex);
  bool _dirty DANNAN_GUARDED_BY(_mutex) = false;
};

}  // namespace dannan::identity
