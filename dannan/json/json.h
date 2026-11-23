//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

// JSON for every project in the stack, through Boost.JSON (TODO/2026-11-20-stack-plan.md, item
// 24; the maintainer, 2026-11-20 and 2026-11-22). Boost.JSON's compiled part is carried once, in
// libdannan (json.cpp): a project includes this header, uses Boost.JSON's own types, and links
// dannan::dannan, never Boost.JSON's library of its own, which would carry it twice.
//
// A value keeps int64 and uint64 apart (kind::int64, kind::uint64): a count above INT64_MAX
// stays a uint64 through a round trip, and a negative stays an int64.

#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dannan::json {

using boost::json::array;
using boost::json::kind;
using boost::json::object;
using boost::json::string;
using boost::json::value;

// A value's compact text, as bytes (no whitespace, keys in the object's order). Waits on
// nothing; thread-safe for distinct values.
std::vector<uint8_t> to_bytes(const value& from);

// One JSON text, all of `text`: the value, or nothing with `why` set to where and what was
// wrong. Never throws for bad input. Waits on nothing; thread-safe.
std::optional<value> parse(std::span<const uint8_t> text, std::string* why = nullptr);
std::optional<value> parse(std::string_view text, std::string* why = nullptr);

}  // namespace dannan::json
