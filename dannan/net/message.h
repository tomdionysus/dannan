//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/net/codec.h"
#include "dannan/net/routes.h"
#include "dannan/sched/retained_memory.h"
#include "dannan/sched/work.h"

namespace dannan::net {

// The retained memory a message holds while it is queued, reassembled or
// handled. It follows the message through every handoff, so no
// asynchronous gap goes uncharged, and is released with the last copy.
using MemoryHold = std::shared_ptr<std::vector<sched::RetainedMemoryLedger::Lease>>;

struct Message {
  MessageType type{};
  Bytes payload;
  MemoryHold memory;
};

// A whole message as it arrived: its request id (0 for a notification, odd
// when the dialler sent it, even when the acceptor did) and its class.
struct Frame {
  uint64_t request_id = 0;
  sched::FrameType frame = sched::FrameType::control;
  Message message;
};

// Why a peer would not answer, as the hosted systems' HTTP APIs say it: a
// status number, a snake_case code and English text. Every answer is one of
// three: an answer, a refusal that stands, or a refusal to try later.
struct Refusal {
  // Zero when the refusal names none.
  uint16_t status = 0;
  std::string code;
  std::string message;
};

// An error reply carrying the refusal. The text goes first, so a reader
// that takes only the text still reads it.
Message error_reply(uint16_t status, std::string_view code, std::string_view message);
// The refusal an error reply carries; empty for any other message.
std::optional<Refusal> refusal_of(const Message& message);
// Whether asking again may get another answer: 429, 500, 502, 503, 504, and
// a refusal that names no status.
bool try_later(const Refusal& refusal) noexcept;

}  // namespace dannan::net
