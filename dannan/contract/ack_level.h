//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

// The stack's ack levels (the maintainer, 2026-11-15; kvasir TODO/2026-11-15-ack-levels-standard.md; the
// plan's item 13): how far a write has gone when the node answers, never where. Ordered
// weakest first, so "at least decided" is a comparison. A level is the least the node waits
// for; the evidence says which replicas had the write. A hosted system may keep a level of its
// own beside these (Dagda's durable), never offered under these names.
namespace dannan::contract {

enum class AckLevel : uint8_t {
  // Answered at once, the write actioned after: lost if the node stops first, or if the
  // cluster later refuses it.
  ephemeral = 0,
  // Answered once one node's disk has it (this node's when it can hold the copy, else the next
  // in placement order): lost if that disk is lost first, or if the cluster later refuses it.
  persisted = 1,
  // Answered once a majority has agreed it, in memory, disks after: lost if a majority of the
  // replicas lose power together. An acceptor that answers from memory makes a write decided.
  decided = 2,
  // Answered once a majority of the replicas have it on disk: never lost while one majority's
  // disks survive. An acceptor that syncs before answering makes a write replicated.
  replicated = 3,
};

// The names, as config and SQL's SET take them: lower case, in order.
const char* ack_level_name(AckLevel level) noexcept;
std::array<std::string_view, 4> ack_level_names() noexcept;
// Exactly one of the four names; anything else (another case included) is not a level.
std::optional<AckLevel> parse_ack_level(std::string_view name) noexcept;

}  // namespace dannan::contract
