//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// The wire encoding every Dannan record uses, byte for byte protocol 23's:
// integers big-endian, a blob or string as a u32 length then its bytes, a
// fixed-size array raw. A decode that cannot be completed throws DecodeError;
// nothing is guessed.
namespace dannan::net {

using Bytes = std::vector<uint8_t>;

class DecodeError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Writer {
 public:
  void u8(uint8_t value) { _data.push_back(value); }
  void u16(uint16_t value);
  void u32(uint32_t value);
  void u64(uint64_t value);
  void i64(int64_t value) { u64(static_cast<uint64_t>(value)); }
  void raw(std::span<const uint8_t> bytes) { _data.insert(_data.end(), bytes.begin(), bytes.end()); }
  // Throws std::length_error past 2^32 - 1 bytes.
  void bytes(std::span<const uint8_t> bytes);
  void string(const std::string& value) { bytes({reinterpret_cast<const uint8_t*>(value.data()), value.size()}); }
  template <size_t N>
  void fixed(const std::array<uint8_t, N>& value) {
    raw(value);
  }

  const Bytes& data() const noexcept { return _data; }
  Bytes take() noexcept { return std::move(_data); }

 private:
  Bytes _data;
};

// Reads from a buffer the caller keeps alive. Every read with a length takes
// a maximum, checked before anything is copied or allocated, so a peer
// cannot make a node allocate what it merely claims.
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> data) noexcept : _data(data) {}
  // A temporary buffer would be gone before the first read.
  explicit Reader(Bytes&&) = delete;

  uint8_t u8();
  uint16_t u16();
  uint32_t u32();
  uint64_t u64();
  int64_t i64() { return static_cast<int64_t>(u64()); }
  Bytes raw(size_t count);
  Bytes bytes(size_t maximum = 256 * 1024 * 1024);
  std::string string(size_t maximum = 65536);
  // Zero-copy: the span borrows the reader's buffer.
  std::span<const uint8_t> view(size_t count);
  std::span<const uint8_t> view_bytes(size_t maximum = 256 * 1024 * 1024);
  template <size_t N>
  std::array<uint8_t, N> fixed() {
    need(N);
    std::array<uint8_t, N> out{};
    std::copy_n(_data.begin() + static_cast<std::ptrdiff_t>(_position), N, out.begin());
    _position += N;
    return out;
  }

  size_t remaining() const noexcept { return _data.size() - _position; }
  // Throws DecodeError if anything is left unread.
  void finish() const;

 private:
  void need(size_t count) const;

  std::span<const uint8_t> _data;
  size_t _position = 0;
};

}  // namespace dannan::net
