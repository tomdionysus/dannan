//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Text forms of bytes, and UTF-8 checks (TODO/2026-11-20-stack-plan.md, item 9; from Lugh's
// core/encoding.h, with Louhi's padded base64url). Every decoder is exact: the alphabet only,
// and no length, padding or trailing bits an encoder would not make, so one value has one
// spelling (a token is compared and hashed as it is written). A decoder takes the buffer to
// decode into, so a secret (a TOTP key) can go straight into identity::SecureBuffer.
namespace dannan::encoding {

using Bytes = std::vector<uint8_t>;

// Lower-case hex.
std::string hex(std::span<const uint8_t> data);
// Hex in either case; nothing for an odd length or any other character.
std::optional<Bytes> unhex(std::string_view text);

enum class Padding : uint8_t { none, padded };

// RFC 4648 base64, the standard alphabet with padding (HTTP Basic, RFC 7617).
std::string base64(std::span<const uint8_t> data);
// RFC 4648 base64url: unpadded for tokens in a URL (a JWS), padded where a format asks (Docker's
// registry auth header).
std::string base64url(std::span<const uint8_t> data, Padding padding = Padding::none);
// RFC 4648 base32, upper case, unpadded, as authenticator apps take a TOTP secret.
std::string base32(std::span<const uint8_t> data);

namespace detail {
// The decoders' one loop, into `out`: `bits` per symbol from `value` (-1 for a character outside
// the alphabet). False for anything not exactly what an encoder makes.
template <class Out, class Value>
bool decode_bits(std::string_view text, int bits_per, Value value, Out& out) {
  unsigned buffer = 0;
  int bits = 0;
  out.reserve(text.size() * static_cast<size_t>(bits_per) / 8);
  for (const char c : text) {
    const int v = value(c);
    if (v < 0) return false;
    buffer = (buffer << bits_per) | static_cast<unsigned>(v);
    bits += bits_per;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
    }
    buffer &= (1u << bits) - 1;
  }
  return bits < bits_per && buffer == 0;
}
int base64url_value(char c) noexcept;
int base32_value(char c) noexcept;
// The unpadded body of a padded base64 or base64url text, or nothing if its padding is wrong.
std::optional<std::string_view> unpadded(std::string_view text) noexcept;
}  // namespace detail

template <class Out = Bytes>
std::optional<Out> base64url_decode(std::string_view text, Padding padding = Padding::none) {
  if (padding == Padding::padded) {
    const auto body = detail::unpadded(text);
    if (!body) return std::nullopt;
    text = *body;
  }
  if (text.size() % 4 == 1) return std::nullopt;
  Out out;
  if (!detail::decode_bits(text, 6, detail::base64url_value, out)) return std::nullopt;
  return out;
}

template <class Out = Bytes>
std::optional<Out> base64_decode(std::string_view text) {
  const auto body = detail::unpadded(text);
  if (!body || body->size() % 4 == 1) return std::nullopt;
  const auto value = [](char c) -> int {
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '-' || c == '_') return -1;
    return detail::base64url_value(c);
  };
  Out out;
  if (!detail::decode_bits(*body, 6, value, out)) return std::nullopt;
  return out;
}

// Either case, no padding.
template <class Out = Bytes>
std::optional<Out> base32_decode(std::string_view text) {
  Out out;
  if (!detail::decode_bits(text, 5, detail::base32_value, out)) return std::nullopt;
  return out;
}

// The length of the UTF-8 sequence (RFC 3629) starting at `at`, or 0 for one that is not valid:
// overlong forms, surrogates and values past U+10FFFF included.
size_t utf8_length(std::string_view text, size_t at) noexcept;
// Whether all of `text` is valid UTF-8.
bool valid_utf8(std::string_view text) noexcept;
// Valid UTF-8 with no control character: no C0 (U+0000 to U+001F), DEL (U+007F) or C1 (U+0080 to
// U+009F). For a name or a label a person reads.
bool printable(std::string_view text) noexcept;

}  // namespace dannan::encoding
