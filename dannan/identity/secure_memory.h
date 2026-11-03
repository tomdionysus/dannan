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

// A secret value: key material, a password, a token. It has no conversion
// to text and no stream operator, so it cannot reach a log by accident; it
// is revealed only by asking. Compared in constant time.
class Secret {
 public:
  explicit Secret(std::span<const uint8_t> bytes);
  explicit Secret(SecureBytes bytes) noexcept : _bytes(std::move(bytes)) {}
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
