//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/encoding/base.h"

namespace dannan::encoding {

std::string hex(std::span<const uint8_t> data) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const auto byte : data) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 15]);
  }
  return out;
}

std::optional<Bytes> unhex(std::string_view text) {
  if (text.size() % 2) return std::nullopt;
  const auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Bytes out;
  out.reserve(text.size() / 2);
  for (size_t i = 0; i < text.size(); i += 2) {
    const int high = value(text[i]);
    const int low = value(text[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    out.push_back(static_cast<uint8_t>(high << 4 | low));
  }
  return out;
}

namespace {

constexpr char url_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64_with(std::span<const uint8_t> data, const char* alphabet, bool pad) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    const unsigned block = (unsigned{data[i]} << 16) | (unsigned{data[i + 1]} << 8) | data[i + 2];
    for (int shift = 18; shift >= 0; shift -= 6) out.push_back(alphabet[(block >> shift) & 0x3f]);
  }
  const auto left = data.size() - i;
  if (left > 0) {
    unsigned block = unsigned{data[i]} << 16;
    if (left == 2) block |= unsigned{data[i + 1]} << 8;
    out.push_back(alphabet[(block >> 18) & 0x3f]);
    out.push_back(alphabet[(block >> 12) & 0x3f]);
    if (left == 2) out.push_back(alphabet[(block >> 6) & 0x3f]);
    if (pad) out.append(3 - left, '=');
  }
  return out;
}

}  // namespace

std::string base64(std::span<const uint8_t> data) { return base64_with(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true); }

std::string base64url(std::span<const uint8_t> data, Padding padding) { return base64_with(data, url_alphabet, padding == Padding::padded); }

std::string base32(std::span<const uint8_t> data) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  std::string out;
  out.reserve((data.size() * 8 + 4) / 5);
  unsigned buffer = 0;
  int bits = 0;
  for (const auto byte : data) {
    buffer = (buffer << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      out.push_back(alphabet[(buffer >> (bits - 5)) & 0x1f]);
      bits -= 5;
    }
    buffer &= (1u << bits) - 1;
  }
  if (bits > 0) out.push_back(alphabet[(buffer << (5 - bits)) & 0x1f]);
  return out;
}

namespace detail {

int base64url_value(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

int base32_value(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a';
  if (c >= '2' && c <= '7') return c - '2' + 26;
  return -1;
}

std::optional<std::string_view> unpadded(std::string_view text) noexcept {
  if (text.size() % 4 != 0) return std::nullopt;
  size_t padding = 0;
  while (padding < text.size() && text[text.size() - 1 - padding] == '=') ++padding;
  if (padding > 2) return std::nullopt;
  return text.substr(0, text.size() - padding);
}

}  // namespace detail

size_t utf8_length(std::string_view text, size_t at) noexcept {
  const auto byte = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
  const auto first = byte(at);
  if (first < 0x80) return 1;
  size_t length;
  unsigned value;
  if ((first & 0xe0) == 0xc0) {
    length = 2;
    value = first & 0x1f;
  } else if ((first & 0xf0) == 0xe0) {
    length = 3;
    value = first & 0x0f;
  } else if ((first & 0xf8) == 0xf0) {
    length = 4;
    value = first & 0x07;
  } else {
    return 0;
  }
  if (at + length > text.size()) return 0;
  for (size_t i = 1; i < length; ++i) {
    if ((byte(at + i) & 0xc0) != 0x80) return 0;
    value = (value << 6) | (byte(at + i) & 0x3f);
  }
  static constexpr unsigned least[] = {0, 0, 0x80, 0x800, 0x10000};
  if (value < least[length] || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return 0;
  return length;
}

bool valid_utf8(std::string_view text) noexcept {
  for (size_t i = 0; i < text.size();) {
    const auto length = utf8_length(text, i);
    if (length == 0) return false;
    i += length;
  }
  return true;
}

bool printable(std::string_view text) noexcept {
  for (size_t i = 0; i < text.size();) {
    const auto length = utf8_length(text, i);
    if (length == 0) return false;
    const auto first = static_cast<unsigned char>(text[i]);
    if (length == 1 && (first < 0x20 || first == 0x7f)) return false;
    // U+0080 to U+009F are C2 80 to C2 9F.
    if (length == 2 && first == 0xc2 && static_cast<unsigned char>(text[i + 1]) <= 0x9f) return false;
    i += length;
  }
  return true;
}

}  // namespace dannan::encoding
