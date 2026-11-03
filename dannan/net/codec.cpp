//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/codec.h"

namespace dannan::net {

void Reader::need(size_t count) const {
  if (count > remaining()) throw DecodeError("truncated input");
}

uint8_t Reader::u8() {
  need(1);
  return _data[_position++];
}

uint16_t Reader::u16() {
  need(2);
  const auto value = static_cast<uint16_t>(_data[_position] << 8 | _data[_position + 1]);
  _position += 2;
  return value;
}

uint32_t Reader::u32() {
  need(4);
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value = (value << 8) | _data[_position++];
  return value;
}

uint64_t Reader::u64() {
  need(8);
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | _data[_position++];
  return value;
}

Bytes Reader::raw(size_t count) {
  const auto bytes = view(count);
  return Bytes(bytes.begin(), bytes.end());
}

Bytes Reader::bytes(size_t maximum) {
  const auto bytes = view_bytes(maximum);
  return Bytes(bytes.begin(), bytes.end());
}

std::span<const uint8_t> Reader::view(size_t count) {
  need(count);
  const auto out = _data.subspan(_position, count);
  _position += count;
  return out;
}

std::span<const uint8_t> Reader::view_bytes(size_t maximum) {
  const auto count = u32();
  if (count > maximum) throw DecodeError("blob too large");
  return view(count);
}

std::string Reader::string(size_t maximum) {
  const auto value = view_bytes(maximum);
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void Reader::finish() const {
  if (_position != _data.size()) throw DecodeError("trailing input");
}

}  // namespace dannan::net
