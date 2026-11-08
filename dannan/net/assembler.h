//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

#include "dannan/net/channel.h"
#include "dannan/net/message.h"
#include "dannan/sched/retained_memory.h"

namespace dannan::net {

// Reassembles one session's incoming fragments into messages, with bounds
// beyond the per-message one: how many messages may be incomplete at once,
// and how many bytes they may hold together. Every fragment's bytes are
// charged to the node's retained-memory ledger as reassembly; the leases
// travel with the finished message. A fragment that breaks a rule throws
// FrameRefused and the session ends. Single owner: the session's reader.
class MessageAssembler {
 public:
  // Throws std::invalid_argument for a zero bound. `memory` may be null.
  MessageAssembler(size_t max_incomplete_messages, size_t max_incomplete_bytes, size_t max_message_bytes, sched::RetainedMemoryLedger* memory = nullptr);

  // The finished message when `fragment` was its last, else empty.
  std::optional<Frame> push(WireFragment fragment);
  // A promotion of an incomplete message to a more urgent class.
  void promote(uint64_t request_id, sched::FrameType frame);
  // Drops an incomplete message (its sender cancelled it).
  void discard(uint64_t request_id);
  // Drops every incomplete message and the memory it holds.
  void clear();

  size_t incomplete_messages() const noexcept { return _partial.size(); }
  size_t incomplete_bytes() const noexcept { return _partial_bytes; }

 private:
  struct Partial {
    sched::FrameType frame = sched::FrameType::control;
    MessageType type{};
    Bytes payload;
    MemoryHold memory;
  };

  void charge(MemoryHold& memory, sched::FrameType frame, size_t bytes);

  std::map<uint64_t, Partial> _partial;
  size_t _partial_bytes = 0;
  const size_t _max_messages;
  const size_t _max_bytes;
  const size_t _max_message;
  sched::RetainedMemoryLedger* _memory;
};

// The more urgent of two frame classes.
constexpr sched::FrameType more_urgent(sched::FrameType a, sched::FrameType b) noexcept { return sched::frame_priority(a) <= sched::frame_priority(b) ? a : b; }

}  // namespace dannan::net
