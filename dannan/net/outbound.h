//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>

#include "dannan/net/message.h"
#include "dannan/sched/retained_memory.h"

namespace dannan::net {

// Per-session limits, protocol 23's. A pipelining caller sizes its window
// against them; the queue binds first, since the writer drains it as it goes.
inline constexpr size_t max_pending_replies = 512;
inline constexpr size_t max_outbound_messages = 256;
inline constexpr size_t max_outbound_bytes = 128ull * 1024 * 1024;
// A message this small (a ping, membership, account or session gossip) may
// use a reserve larger ones cannot, so no number of large messages crowds a
// small one out.
inline constexpr size_t small_message = 64 * 1024;
inline constexpr size_t small_reserve_messages = 32;
inline constexpr size_t small_reserve_bytes = 8ull * 1024 * 1024;
// Substitutes for refused replies may pass the queue's own limit, one per
// reply a caller can be awaiting; beyond that the session closes, which
// answers every caller on it.
inline constexpr size_t max_outbound_with_refusals = max_outbound_messages + max_pending_replies;

// Why a message of `payload` bytes may not join a queue holding `queued`
// messages of `queued_bytes`; null when it may.
const char* outbound_refusal(size_t queued, size_t queued_bytes, size_t payload) noexcept;

// A request or notification the queue would not take, and why.
class OutboundRefused : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// One session's outbound messages, chosen frame by frame: the most urgent
// class first (law 1: control never queues behind data), equal classes
// taking turns by frame, so a long transfer never holds the lane and urgent
// work overtakes it between its frames. A reply to an admitted request is
// never dropped for capacity: a 503 transport_busy goes in its place.
//
// Not synchronised: owned by the session's strand. The writer takes a
// fragment with next(), writes it, then calls written(); a promotion or
// cancellation in between applies to the rest of that message.
class OutboundQueue {
 public:
  // Told once whether a notification went out (true) or was abandoned.
  using Sent = std::function<void(bool sent)>;

  struct Fragment {
    uint64_t request_id = 0;
    sched::FrameType frame = sched::FrameType::control;
    MessageType type{};
    bool first = false;
    bool last = false;
    std::span<const uint8_t> payload;
  };

  // `memory` may be null; queued messages are charged to it as rpc frames.
  explicit OutboundQueue(sched::RetainedMemoryLedger* memory = nullptr) : _memory(memory) {}

  // Queues a request (an odd or even id), a notification (id 0) or a reply.
  // A request or notification the queue or the memory ledger refuses throws
  // OutboundRefused with the reason; a reply is substituted, and throws only
  // past max_outbound_with_refusals. False when the reply's request was
  // cancelled and nothing was queued.
  bool push(uint64_t request_id, sched::FrameType frame, Message message, bool reply, Sent sent = {});

  // Inbound requests whose replies this queue will carry: their class
  // (which the reply travels at), a promotion, and a cancellation, after
  // which the reply is never sent. Only a registered request is cancelled.
  void register_inbound(uint64_t request_id, sched::FrameType frame);
  void promote_inbound(uint64_t request_id, sched::FrameType frame);
  void cancel_inbound(uint64_t request_id);

  // This side's own requests: a promotion, or a cancellation that removes
  // the request even if part of it has been written.
  void promote_outgoing(uint64_t request_id, sched::FrameType frame);
  void cancel_outgoing(uint64_t request_id);

  // The next fragment to write, at most `max_frame` bytes of payload, or
  // none. Its payload stays valid until written().
  std::optional<Fragment> next(size_t max_frame);
  // The fragment from next() has been written.
  void written();
  // The session is closing: every waiting notification is told it was not
  // sent, and everything is dropped.
  void abandon();

  size_t size() const noexcept { return _queue.size() + (_in_flight ? 1 : 0); }
  uint64_t bytes() const noexcept { return _bytes; }
  bool writing() const noexcept { return _in_flight.has_value(); }
  // Nothing queued or being written, and no inbound request awaiting its reply.
  bool drained() const noexcept { return _queue.empty() && !_in_flight && _inbound.empty(); }

 private:
  struct Item {
    uint64_t request_id = 0;
    sched::FrameType frame = sched::FrameType::control;
    Message message;
    size_t offset = 0;
    bool reply = false;
    Sent sent;
    sched::RetainedMemoryLedger::Lease memory;
    size_t written_now = 0;
  };

  void finish(Item& item, bool sent);

  sched::RetainedMemoryLedger* _memory;
  std::deque<Item> _queue;
  std::optional<Item> _in_flight;
  uint64_t _bytes = 0;
  std::map<uint64_t, sched::FrameType> _inbound;
  std::set<uint64_t> _cancelled_inbound;
  std::map<uint64_t, sched::FrameType> _outgoing;
  std::set<uint64_t> _cancelled_outgoing;
};

}  // namespace dannan::net
