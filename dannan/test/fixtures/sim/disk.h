//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dannan/storage/file_system.h"
#include "dannan/test/fixtures/sim/world.h"

// The simulated disk (TODO/2026-11-15-file-system-interface.md, "The simulated disk"): the
// stack's file-system interface over memory, with what a real disk does to a crash made exact.
// Each file has its current image and its durable one, and each directory its current entries
// and its durable ones: a file's sync makes its data durable, a directory's sync its entries
// (an unsynced create, rename or remove may be lost), sync_all both for everything. A crash
// keeps only what was durable, and a file whose directory's entry was lost goes with it. Faults
// drawn from the seed: a sync that fails (then, half the time, dropping the dirty pages as Linux
// may), a sync that lies, a crash that tears a file's unsynced growth to a prefix, a disk that is
// full, and one cut after a given number of operations, as a power cut at that instant.
namespace dannan::test::sim {

class Disk final : public storage::FileSystem {
 public:
  explicit Disk(Random& random, uint64_t capacity = std::numeric_limits<uint64_t>::max()) : _state(std::make_shared<State>(random, capacity)) {}

  // Faults, as the seed says.
  // One sync in this many fails (zero: none).
  void fail_syncs_one_in(uint64_t in) {
    std::lock_guard lock(_state->mutex);
    _state->fail_one_in = in;
  }
  // Every sync reports success and keeps nothing.
  void lie_on_syncs(bool lie) {
    std::lock_guard lock(_state->mutex);
    _state->lying = lie;
  }
  // After this many more operations that change something, the power is cut: that one and every
  // later operation fails (io) until crash().
  void cut_after(uint64_t operations) {
    std::lock_guard lock(_state->mutex);
    _state->cut_after = operations;
  }
  bool is_cut() const {
    std::lock_guard lock(_state->mutex);
    return _state->cut;
  }
  // Operations that changed something so far, for a test that cuts at each in turn.
  uint64_t operations() const {
    std::lock_guard lock(_state->mutex);
    return _state->operations;
  }
  // Only what was durable remains. `tear`: a file grown past its durable image keeps the durable
  // image and a prefix of the growth, its length drawn from the seed. Open files and locks from
  // before are gone.
  void crash(bool tear = false) {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    s.names = s.durable_names;
    // A name whose directory did not survive goes too; a parent sorts before its children.
    for (auto it = s.names.begin(); it != s.names.end();) {
      const auto parent = it->first.parent_path();
      if (!root(parent) && !(s.names.contains(parent) && s.names.at(parent).directory)) {
        it = s.names.erase(it);
        continue;
      }
      ++it;
    }
    std::set<uint64_t> kept;
    for (const auto& [path, name] : s.names)
      if (!name.directory) kept.insert(name.inode);
    for (auto it = s.inodes.begin(); it != s.inodes.end();) {
      if (!kept.contains(it->first)) {
        it = s.inodes.erase(it);
        continue;
      }
      auto& inode = it->second;
      const bool grew = inode.data.size() > inode.durable.size() && std::equal(inode.durable.begin(), inode.durable.end(), inode.data.begin());
      if (tear && grew) {
        const auto extra = inode.data.size() - inode.durable.size();
        inode.data.resize(inode.durable.size() + s.random.below(extra + 1));
      } else {
        inode.data = inode.durable;
      }
      ++it;
    }
    s.locks.clear();
    ++s.generation;
    s.cut = false;
    s.cut_after.reset();
  }
  uint64_t used() const {
    std::lock_guard lock(_state->mutex);
    return _state->used();
  }

  storage::Result<std::unique_ptr<storage::File>> open(const storage::Path& path_in, storage::OpenMode mode) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto path = normal(path_in);
    if (s.cut) return fault("open", path, EIO);
    if (!s.directory(path.parent_path())) return fault("open", path, ENOENT);
    auto found = s.names.find(path);
    if (found != s.names.end()) {
      if (found->second.directory) return fault("open", path, EISDIR);
      if (mode.create && mode.exclusive) return fault("open", path, EEXIST);
      if (mode.truncate) {
        if (auto cut = s.step("open", path)) return *cut;
        s.inodes[found->second.inode].data.clear();
      }
      return file(found->second.inode, path, mode);
    }
    if (!mode.create) return fault("open", path, ENOENT);
    if (auto cut = s.step("open", path)) return *cut;
    const auto inode = s.next_inode++;
    s.inodes[inode];
    s.names[path] = Name{false, inode};
    return file(inode, path, mode);
  }

  storage::Result<void> rename(const storage::Path& from_in, const storage::Path& to_in) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto from = normal(from_in), to = normal(to_in);
    if (s.cut) return fault("rename", from, EIO);
    const auto found = s.names.find(from);
    if (found == s.names.end() || !s.directory(to.parent_path())) return fault("rename", from, ENOENT);
    if (found->second.directory) return fault("rename", from, EISDIR);
    if (const auto target = s.names.find(to); target != s.names.end() && target->second.directory) return fault("rename", to, EISDIR);
    if (auto cut = s.step("rename", from)) return *cut;
    s.names[to] = found->second;
    s.names.erase(from);
    return {};
  }

  storage::Result<void> link(const storage::Path& from_in, const storage::Path& to_in) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto from = normal(from_in), to = normal(to_in);
    if (s.cut) return fault("link", to, EIO);
    const auto found = s.names.find(from);
    if (found == s.names.end() || !s.directory(to.parent_path())) return fault("link", to, ENOENT);
    if (found->second.directory) return fault("link", to, EPERM);
    if (s.names.contains(to)) return fault("link", to, EEXIST);
    if (auto cut = s.step("link", to)) return *cut;
    s.names[to] = found->second;
    return {};
  }

  storage::Result<void> remove(const storage::Path& path_in) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto path = normal(path_in);
    if (s.cut) return fault("remove", path, EIO);
    const auto found = s.names.find(path);
    if (found == s.names.end()) return fault("remove", path, ENOENT);
    if (found->second.directory)
      for (const auto& [other, name] : s.names)
        if (other.parent_path() == path) return fault("remove", path, ENOTEMPTY);
    if (auto cut = s.step("remove", path)) return *cut;
    s.names.erase(found);
    return {};
  }

  storage::Result<std::vector<storage::Entry>> list(const storage::Path& directory_in) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto directory = normal(directory_in);
    if (s.cut) return fault("list", directory, EIO);
    if (!s.directory(directory)) return fault("list", directory, s.names.contains(directory) ? ENOTDIR : ENOENT);
    std::vector<storage::Entry> out;
    for (const auto& [path, name] : s.names)
      if (path.parent_path() == directory) out.push_back(s.entry(path, name));
    std::sort(out.begin(), out.end(), [](const storage::Entry& a, const storage::Entry& b) { return a.name < b.name; });
    return out;
  }

  storage::Result<storage::Entry> stat(const storage::Path& path_in) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto path = normal(path_in);
    if (s.cut) return fault("stat", path, EIO);
    if (root(path)) return storage::Entry{path.string(), storage::EntryType::directory, 0};
    const auto found = s.names.find(path);
    if (found == s.names.end()) return fault("stat", path, ENOENT);
    return s.entry(path, found->second);
  }

  storage::Result<void> make_directories(const storage::Path& path_in, unsigned = 0700) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto path = normal(path_in);
    if (s.cut) return fault("make directory", path, EIO);
    storage::Path made;
    for (const auto& part : path) {
      made /= part;
      if (root(made)) continue;
      const auto found = s.names.find(made);
      if (found != s.names.end()) {
        if (!found->second.directory) return fault("make directory", made, EEXIST);
        continue;
      }
      if (auto cut = s.step("make directory", made)) return *cut;
      s.names[made] = Name{true, 0};
    }
    return {};
  }

  storage::Result<void> sync_directory(const storage::Path& directory_in) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    const auto directory = normal(directory_in);
    if (s.cut) return fault("sync directory", directory, EIO);
    if (!s.directory(directory)) return fault("open directory", directory, ENOENT);
    if (auto cut = s.step("sync directory", directory)) return *cut;
    if (s.lying) return {};
    if (s.fails()) return fault("sync directory", directory, EIO);
    for (auto it = s.durable_names.begin(); it != s.durable_names.end();) it = it->first.parent_path() == directory ? s.durable_names.erase(it) : std::next(it);
    for (const auto& [path, name] : s.names)
      if (path.parent_path() == directory) s.durable_names[path] = name;
    return {};
  }

  storage::Result<void> sync_all(const storage::Path& any_on_it) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    if (s.cut) return fault("sync all", any_on_it, EIO);
    if (auto cut = s.step("sync all", any_on_it)) return *cut;
    if (s.lying) return {};
    if (s.fails()) {
      s.drop_dirty_maybe();
      return fault("sync all", any_on_it, EIO);
    }
    s.durable_names = s.names;
    for (auto& [id, inode] : s.inodes) inode.durable = inode.data;
    return {};
  }

  storage::Result<storage::Space> space(const storage::Path& any_on_it) override {
    std::lock_guard lock(_state->mutex);
    auto& s = *_state;
    if (s.cut) return fault("space", any_on_it, EIO);
    const auto used = s.used();
    return storage::Space{s.capacity > used ? s.capacity - used : 0, s.capacity};
  }

  storage::Result<std::unique_ptr<storage::Lock>> lock(const storage::Path& path_in) override {
    std::lock_guard guard(_state->mutex);
    auto& s = *_state;
    const auto path = normal(path_in);
    if (s.cut) return fault("open lock", path, EIO);
    if (!s.directory(path.parent_path())) return fault("open lock", path, ENOENT);
    if (!s.names.contains(path)) {
      if (auto cut = s.step("open lock", path)) return *cut;
      const auto inode = s.next_inode++;
      s.inodes[inode];
      s.names[path] = Name{false, inode};
    }
    if (s.locks.contains(path)) return fault("lock", path, EWOULDBLOCK);
    s.locks.insert(path);
    return std::unique_ptr<storage::Lock>(std::make_unique<SimLock>(_state, path, s.generation));
  }

 private:
  struct Inode {
    std::vector<uint8_t> data;
    std::vector<uint8_t> durable;
  };
  struct Name {
    bool directory = false;
    uint64_t inode = 0;
  };
  struct State {
    State(Random& r, uint64_t c) : random(r), capacity(c) {}
    std::mutex mutex;
    Random& random;
    uint64_t capacity;
    uint64_t fail_one_in = 0;
    bool lying = false;
    std::optional<uint64_t> cut_after;
    bool cut = false;
    uint64_t operations = 0;
    uint64_t generation = 0;
    uint64_t next_inode = 1;
    std::map<uint64_t, Inode> inodes;
    std::map<storage::Path, Name> names;
    std::map<storage::Path, Name> durable_names;
    std::set<storage::Path> locks;

    bool directory(const storage::Path& path) const {
      if (root(path)) return true;
      const auto found = names.find(path);
      return found != names.end() && found->second.directory;
    }
    // One operation that changes something: refused if the power is cut, or cut now.
    std::optional<storage::Error> step(const char* operation, const storage::Path& path) {
      if (cut) return error(operation, path, EIO);
      if (cut_after) {
        if (*cut_after == 0) {
          cut = true;
          return error(operation, path, EIO);
        }
        --*cut_after;
      }
      ++operations;
      return std::nullopt;
    }
    bool fails() { return fail_one_in && random.one_in(fail_one_in); }
    // A failed sync: Linux may drop the dirty pages and report the next sync good.
    void drop_dirty_maybe() {
      for (auto& [id, inode] : inodes)
        if (random.one_in(2)) inode.data = inode.durable;
    }
    uint64_t used() const {
      std::set<uint64_t> named;
      for (const auto& [path, name] : names)
        if (!name.directory) named.insert(name.inode);
      uint64_t out = 0;
      for (const auto id : named) out += inodes.at(id).data.size();
      return out;
    }
    storage::Entry entry(const storage::Path& path, const Name& name) const {
      if (name.directory) return storage::Entry{path.filename().string(), storage::EntryType::directory, 0};
      return storage::Entry{path.filename().string(), storage::EntryType::file, inodes.at(name.inode).data.size()};
    }
  };

  class SimFile final : public storage::File {
   public:
    SimFile(std::shared_ptr<State> state, uint64_t inode, storage::Path path, storage::OpenMode mode, uint64_t generation)
        : _state(std::move(state)), _inode(inode), _path(std::move(path)), _mode(mode), _generation(generation) {}

    storage::Result<size_t> read_at(uint64_t offset, std::span<uint8_t> into) override {
      std::lock_guard lock(_state->mutex);
      if (auto dead = gone("read")) return *dead;
      const auto& data = _state->inodes.at(_inode).data;
      if (offset >= data.size()) return size_t{0};
      const auto n = std::min<uint64_t>(into.size(), data.size() - offset);
      std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset), n, into.begin());
      return static_cast<size_t>(n);
    }
    storage::Result<size_t> write_at(uint64_t offset, std::span<const uint8_t> bytes) override {
      std::lock_guard lock(_state->mutex);
      return write(offset, bytes);
    }
    storage::Result<size_t> append(std::span<const uint8_t> bytes) override {
      std::lock_guard lock(_state->mutex);
      if (auto dead = gone("write")) return *dead;
      return write(_state->inodes.at(_inode).data.size(), bytes);
    }
    storage::Result<void> truncate(uint64_t size) override {
      std::lock_guard lock(_state->mutex);
      if (auto dead = gone("truncate")) return *dead;
      if (!_mode.write) return error("truncate", _path, EBADF);
      auto& data = _state->inodes.at(_inode).data;
      if (size > data.size() && _state->used() + (size - data.size()) > _state->capacity) return error("truncate", _path, ENOSPC);
      if (auto cut = _state->step("truncate", _path)) return *cut;
      data.resize(size);
      return {};
    }
    storage::Result<uint64_t> size() override {
      std::lock_guard lock(_state->mutex);
      if (auto dead = gone("stat")) return *dead;
      return static_cast<uint64_t>(_state->inodes.at(_inode).data.size());
    }
    storage::Result<void> sync() override {
      std::lock_guard lock(_state->mutex);
      if (auto dead = gone("sync")) return *dead;
      if (auto cut = _state->step("sync", _path)) return *cut;
      auto& inode = _state->inodes.at(_inode);
      if (_state->lying) return {};
      if (_state->fails()) {
        if (_state->random.one_in(2)) inode.data = inode.durable;
        return error("sync", _path, EIO);
      }
      inode.durable = inode.data;
      return {};
    }
    storage::Result<void> close() override { return {}; }

   private:
    std::optional<storage::Error> gone(const char* operation) const {
      if (_state->cut || _state->generation != _generation || !_state->inodes.contains(_inode)) return error(operation, _path, EIO);
      return std::nullopt;
    }
    storage::Result<size_t> write(uint64_t offset, std::span<const uint8_t> bytes) {
      if (auto dead = gone("write")) return *dead;
      if (!_mode.write) return error("write", _path, EBADF);
      auto& data = _state->inodes.at(_inode).data;
      const auto end = offset + bytes.size();
      if (end > data.size() && _state->used() + (end - data.size()) > _state->capacity) return error("write", _path, ENOSPC);
      if (auto cut = _state->step("write", _path)) return *cut;
      if (end > data.size()) data.resize(end);
      std::copy(bytes.begin(), bytes.end(), data.begin() + static_cast<std::ptrdiff_t>(offset));
      return bytes.size();
    }

    std::shared_ptr<State> _state;
    const uint64_t _inode;
    const storage::Path _path;
    const storage::OpenMode _mode;
    const uint64_t _generation;
  };

  class SimLock final : public storage::Lock {
   public:
    SimLock(std::shared_ptr<State> state, storage::Path path, uint64_t generation)
        : _state(std::move(state)), _path(std::move(path)), _generation(generation) {}
    ~SimLock() override {
      std::lock_guard lock(_state->mutex);
      if (_state->generation == _generation) _state->locks.erase(_path);
    }

   private:
    std::shared_ptr<State> _state;
    storage::Path _path;
    uint64_t _generation;
  };

  static bool root(const storage::Path& path) { return path.empty() || path == path.root_path(); }
  static storage::Path normal(const storage::Path& path) {
    auto out = path.lexically_normal();
    if (!out.empty() && out.filename().empty() && !root(out)) out = out.parent_path();
    return out;
  }
  static storage::Error error(std::string operation, const storage::Path& path, int code) {
    return storage::Error{std::move(operation), path, code, code == EIO ? storage::ErrorClass::io : storage::class_of(code)};
  }
  static storage::Error fault(std::string operation, const storage::Path& path, int code) { return error(std::move(operation), path, code); }
  storage::Result<std::unique_ptr<storage::File>> file(uint64_t inode, const storage::Path& path, storage::OpenMode mode) {
    return std::unique_ptr<storage::File>(std::make_unique<SimFile>(_state, inode, path, mode, _state->generation));
  }

  std::shared_ptr<State> _state;
};

}  // namespace dannan::test::sim
