//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/contract/completion.h"

#include "dannan/contract/ack_level.h"

namespace dannan::contract {

const char* fault_name(Fault fault) noexcept {
  switch (fault) {
    case Fault::none:
      return "none";
    case Fault::not_found:
      return "not_found";
    case Fault::conflict:
      return "conflict";
    case Fault::duplicate:
      return "duplicate";
    case Fault::invalid:
      return "invalid";
    case Fault::refused:
      return "refused";
    case Fault::unavailable:
      return "unavailable";
    case Fault::timed_out:
      return "timed_out";
    case Fault::failed:
      return "failed";
  }
  return "unknown";
}

namespace {
constexpr std::array<std::string_view, 4> ack_names{"ephemeral", "persisted", "decided", "replicated"};
}  // namespace

const char* ack_level_name(AckLevel level) noexcept {
  const auto index = static_cast<size_t>(level);
  return index < ack_names.size() ? ack_names[index].data() : "unknown";
}

std::array<std::string_view, 4> ack_level_names() noexcept { return ack_names; }

std::optional<AckLevel> parse_ack_level(std::string_view name) noexcept {
  for (size_t i = 0; i < ack_names.size(); ++i)
    if (ack_names[i] == name) return static_cast<AckLevel>(i);
  return std::nullopt;
}

}  // namespace dannan::contract
