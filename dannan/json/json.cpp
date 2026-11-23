//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/json/json.h"

// Boost.JSON's compiled part, once for the whole stack: every project links it from here.
#include <boost/json/src.hpp>

namespace dannan::json {

std::vector<uint8_t> to_bytes(const value& from) {
  const auto text = boost::json::serialize(from);
  return {text.begin(), text.end()};
}

std::optional<value> parse(std::string_view text, std::string* why) {
  boost::system::error_code error;
  auto out = boost::json::parse(text, error);
  if (error) {
    if (why) *why = error.message();
    return std::nullopt;
  }
  return out;
}

std::optional<value> parse(std::span<const uint8_t> text, std::string* why) {
  return parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), why);
}

}  // namespace dannan::json
