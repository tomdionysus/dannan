//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>

// Percent-encoding (RFC 3986, section 2.1), the stack's one copy (Tom,
// 2026-11-04: "it should absolutely be in Dannan. everyone else should use
// it"). Byte-oriented: UTF-8 passes through as bytes. Upper-case hex out,
// either case in.
namespace dannan::encoding {

enum class Decoding : uint8_t {
  // RFC 3986: only %XX is decoded; '+' is a literal '+' (MQTT's wildcard,
  // a path's plus).
  strict,
  // application/x-www-form-urlencoded: '+' is a space, and %XX as above.
  form,
};

// Decodes `text`. Throws std::invalid_argument for a malformed escape ('%'
// not followed by two hex digits, as in "%zz" or "100%"): never passed
// through.
std::string percent_decode(std::string_view text, Decoding mode = Decoding::strict);

// The bytes an encoding leaves literal; everything else becomes %XX. '%' is
// always encoded, and so is '+' where a space is written as '+', so every
// encoding decodes back to its input.
class PercentSet {
 public:
  // RFC 3986's unreserved characters: A-Z a-z 0-9 - . _ ~
  static PercentSet unreserved();
  // application/x-www-form-urlencoded (WHATWG): A-Z a-z 0-9 * - . _ left
  // literal, and a space written as '+'. Decode with Decoding::form.
  static PercentSet form();
  // Printable ASCII, 0x21 to 0x7e: space, controls and non-ASCII encoded.
  static PercentSet printable_ascii();

  // Leaves these literal too.
  PercentSet& keep(std::string_view characters);
  // Encodes these too.
  PercentSet& encode(std::string_view characters);

  bool literal(uint8_t byte) const noexcept;
  bool space_as_plus() const noexcept { return _space_as_plus; }

 private:
  std::bitset<256> _literal;
  bool _space_as_plus = false;
};

std::string percent_encode(std::string_view bytes, const PercentSet& set = PercentSet::unreserved());

}  // namespace dannan::encoding
