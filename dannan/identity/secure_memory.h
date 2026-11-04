//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Memory for key material, for the whole stack: Dannan's cluster and node
// keys, Dagda's domain keys, Lugh's master key and signing keys. One home,
// so no project keeps its own (Tom, 2026-11-03).
namespace dannan::identity {

// Bytes on pages of their own: locked in RAM so they are never swapped
// (where the process may lock memory), kept out of core dumps and out of
// forked children (where the platform offers it), and wiped before the
// pages are released. Fixed size, zeroed at the start. Movable, never
// copied. Not thread-safe; owned by one component.
class SecureBytes {
 public:
  // Throws std::runtime_error if the pages cannot be mapped. Locking is best
  // effort and reported by locked().
  explicit SecureBytes(size_t size);
  // Empty: maps nothing, costs nothing.
  SecureBytes() noexcept = default;
  ~SecureBytes();
  SecureBytes(SecureBytes&& other) noexcept;
  SecureBytes& operator=(SecureBytes&& other) noexcept;
  SecureBytes(const SecureBytes&) = delete;
  SecureBytes& operator=(const SecureBytes&) = delete;

  uint8_t* data() noexcept { return _data; }
  const uint8_t* data() const noexcept { return _data; }
  size_t size() const noexcept { return _size; }
  std::span<uint8_t> span() noexcept { return {_data, _size}; }
  std::span<const uint8_t> span() const noexcept { return {_data, _size}; }
  // Whether the pages are locked in RAM.
  bool locked() const noexcept { return _locked; }

 private:
  void release() noexcept;

  uint8_t* _data = nullptr;
  size_t _size = 0;
  size_t _mapped = 0;
  bool _locked = false;
};

// Allocates as SecureBytes does: pages of its own, locked, out of core
// dumps and forks, wiped before they are released. For a buffer that grows
// (a record being encoded, a passphrase typed a character at a time); a
// buffer that moves to larger pages wipes the old ones.
template <class T>
struct SecureAllocator {
  using value_type = T;
  SecureAllocator() noexcept = default;
  template <class U>
  SecureAllocator(const SecureAllocator<U>&) noexcept {}
  T* allocate(size_t count);
  void deallocate(T* pointer, size_t count) noexcept;
  template <class U>
  bool operator==(const SecureAllocator<U>&) const noexcept {
    return true;
  }
};

// The pages behind SecureAllocator.
void* secure_allocate(size_t bytes);
void secure_deallocate(void* pointer, size_t bytes) noexcept;

template <class T>
T* SecureAllocator<T>::allocate(size_t count) {
  return static_cast<T*>(secure_allocate(count * sizeof(T)));
}

template <class T>
void SecureAllocator<T>::deallocate(T* pointer, size_t count) noexcept {
  secure_deallocate(pointer, count * sizeof(T));
}

// A growable byte buffer on secure pages.
using SecureBuffer = std::vector<uint8_t, SecureAllocator<uint8_t>>;

// A secret value: key material, a password, a token. It has no conversion
// to text and no stream operator, so it cannot reach a log by accident; it
// is revealed only by asking. Compared in constant time.
class Secret {
 public:
  // Empty (size 0, nothing mapped), so a struct or a result can hold one
  // before it has a value.
  Secret() noexcept = default;
  explicit Secret(std::span<const uint8_t> bytes);
  explicit Secret(SecureBytes bytes) noexcept : _bytes(std::move(bytes)) {}
  // From a growable secure buffer (a passphrase typed a character at a
  // time); the buffer is wiped.
  explicit Secret(SecureBuffer&& buffer);
  // `size` fresh random bytes. Throws std::runtime_error if the generator
  // fails.
  static Secret random(size_t size);

  std::span<const uint8_t> reveal() const noexcept { return _bytes.span(); }
  size_t size() const noexcept { return _bytes.size(); }
  bool locked() const noexcept { return _bytes.locked(); }

  // Constant time in the length of the shorter; secrets of different
  // lengths are unequal.
  friend bool operator==(const Secret& a, const Secret& b) noexcept;

 private:
  SecureBytes _bytes;
};

// How many secure allocations (SecureBytes, SecureBuffer, everything on
// SecureAllocator) could not be locked into RAM since the process started:
// their pages may be swapped. A node says so once, at start, after loading
// its keys (RLIMIT_MEMLOCK is the usual cause). Thread-safe.
uint64_t unlocked_secure_allocations() noexcept;

// What harden_process() managed.
struct Hardening {
  bool core_dumps_disabled = false;
  bool attach_refused = false;
  std::string detail;
};

// Process-wide, once at start, by a node that will hold keys: no core
// file, and no attaching by another process of the same user where the
// platform can refuse it. Never fails; reports what it could not do.
Hardening harden_process() noexcept;

}  // namespace dannan::identity
