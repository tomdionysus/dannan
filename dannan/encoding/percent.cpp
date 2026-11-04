//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/encoding/percent.h"

#include <stdexcept>

namespace dannan::encoding {

namespace {

int hex_value(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

PercentSet with(std::string_view characters) {
  PercentSet out;
  out.keep(characters);
  return out;
}

constexpr std::string_view alphanumerics = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

}  // namespace

std::string percent_decode(std::string_view text, Decoding mode) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '%') {
      const int high = i + 2 < text.size() ? hex_value(text[i + 1]) : -1;
      const int low = i + 2 < text.size() ? hex_value(text[i + 2]) : -1;
      if (high < 0 || low < 0) throw std::invalid_argument("malformed percent escape at offset " + std::to_string(i) + " of \"" + std::string(text) + "\"");
      out += static_cast<char>((high << 4) | low);
      i += 2;
    } else if (c == '+' && mode == Decoding::form) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

PercentSet PercentSet::unreserved() {
  auto out = with(alphanumerics);
  out.keep("-._~");
  return out;
}

PercentSet PercentSet::form() {
  auto out = with(alphanumerics);
  out.keep("*-._");
  out._space_as_plus = true;
  return out;
}

PercentSet PercentSet::printable_ascii() {
  PercentSet out;
  for (int byte = 0x21; byte <= 0x7e; ++byte) out._literal.set(static_cast<size_t>(byte));
  return out;
}

PercentSet& PercentSet::keep(std::string_view characters) {
  for (const char c : characters) _literal.set(static_cast<uint8_t>(c));
  return *this;
}

PercentSet& PercentSet::encode(std::string_view characters) {
  for (const char c : characters) _literal.reset(static_cast<uint8_t>(c));
  return *this;
}

bool PercentSet::literal(uint8_t byte) const noexcept {
  if (byte == '%') return false;
  if (byte == '+' && _space_as_plus) return false;
  return _literal.test(byte);
}

std::string percent_encode(std::string_view bytes, const PercentSet& set) {
  static constexpr char digits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(bytes.size());
  for (const char c : bytes) {
    const auto byte = static_cast<uint8_t>(c);
    if (set.literal(byte)) {
      out += c;
    } else if (byte == ' ' && set.space_as_plus()) {
      out += '+';
    } else {
      out += '%';
      out += digits[byte >> 4];
      out += digits[byte & 0x0f];
    }
  }
  return out;
}

}  // namespace dannan::encoding
