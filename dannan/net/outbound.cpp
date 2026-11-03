//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/outbound.h"

#include <algorithm>
#include <string>

#include "dannan/net/assembler.h"

namespace dannan::net {
namespace {

constexpr const char* memory_refusal = "process retained-memory RPC admission saturated";

Message reply_not_sent(const char* why) { return error_reply(503, "transport_busy", std::string("reply not sent: ") + why); }

}  // namespace

const char* outbound_refusal(size_t queued, size_t queued_bytes, size_t payload) noexcept {
  const bool small = payload <= small_message;
  const size_t message_cap = small ? max_outbound_messages : max_outbound_messages - small_reserve_messages;
  const size_t byte_cap = small ? max_outbound_bytes : max_outbound_bytes - small_reserve_bytes;
  if (queued >= message_cap) return "peer outbound queue full";
  if (payload > byte_cap || queued_bytes > byte_cap - payload) return "peer outbound byte queue full";
  return nullptr;
}

bool OutboundQueue::push(uint64_t request_id, sched::FrameType frame, Message message, bool reply, Sent sent) {
  if (reply) {
    if (_cancelled_inbound.erase(request_id)) {
      _inbound.erase(request_id);
      return false;
    }
    if (const auto found = _inbound.find(request_id); found != _inbound.end()) frame = more_urgent(found->second, frame);
  }
  std::optional<sched::RetainedMemoryLedger::Lease> lease;
  const char* refused = nullptr;
  if (_memory) {
    lease = _memory->try_acquire(sched::work_class(frame), sched::MemoryOwner::rpc_frame, sizeof(Item) + message.payload.size());
    if (!lease) refused = memory_refusal;
  }
  if (!refused) refused = outbound_refusal(size(), _bytes, message.payload.size());
  if (refused) {
    if (!reply) throw OutboundRefused(refused);
    if (size() >= max_outbound_with_refusals) throw OutboundRefused("peer outbound queue full of refusals");
    message = reply_not_sent(refused);
    lease.reset();
  }
  if (!reply && request_id) _outgoing[request_id] = frame;
  _bytes += message.payload.size();
  Item item;
  item.request_id = request_id;
  item.frame = frame;
  item.message = std::move(message);
  item.reply = reply;
  item.sent = std::move(sent);
  if (lease) item.memory = std::move(*lease);
  _queue.push_back(std::move(item));
  return true;
}

void OutboundQueue::register_inbound(uint64_t request_id, sched::FrameType frame) { _inbound[request_id] = frame; }

void OutboundQueue::promote_inbound(uint64_t request_id, sched::FrameType frame) {
  if (frame != sched::FrameType::foreground && frame != sched::FrameType::read_ahead) return;
  if (const auto found = _inbound.find(request_id); found != _inbound.end()) found->second = more_urgent(frame, found->second);
  for (auto& item : _queue)
    if (item.reply && item.request_id == request_id) item.frame = more_urgent(frame, item.frame);
  if (_in_flight && _in_flight->reply && _in_flight->request_id == request_id) _in_flight->frame = more_urgent(frame, _in_flight->frame);
}

void OutboundQueue::cancel_inbound(uint64_t request_id) {
  // Untrusted input: only a registered request makes a tombstone, so the
  // set is bounded by real work in flight.
  if (!_inbound.contains(request_id)) return;
  _inbound.erase(request_id);
  _cancelled_inbound.insert(request_id);
  std::erase_if(_queue, [&](Item& item) {
    if (!item.reply || item.request_id != request_id) return false;
    _bytes -= item.message.payload.size();
    return true;
  });
}

void OutboundQueue::promote_outgoing(uint64_t request_id, sched::FrameType frame) {
  if (const auto found = _outgoing.find(request_id); found != _outgoing.end()) found->second = more_urgent(frame, found->second);
  for (auto& item : _queue)
    if (!item.reply && item.request_id == request_id) item.frame = more_urgent(frame, item.frame);
  if (_in_flight && !_in_flight->reply && _in_flight->request_id == request_id) _in_flight->frame = more_urgent(frame, _in_flight->frame);
}

void OutboundQueue::cancel_outgoing(uint64_t request_id) {
  std::erase_if(_queue, [&](Item& item) {
    if (item.reply || item.request_id != request_id) return false;
    _bytes -= item.message.payload.size();
    return true;
  });
  _outgoing.erase(request_id);
  if (_in_flight && !_in_flight->reply && _in_flight->request_id == request_id) _cancelled_outgoing.insert(request_id);
}

std::optional<OutboundQueue::Fragment> OutboundQueue::next(size_t max_frame) {
  if (_in_flight || _queue.empty()) return std::nullopt;
  // The first of the most urgent: equal classes take turns, since a
  // partly written message goes to the back.
  const auto best = std::min_element(_queue.begin(), _queue.end(),
                                     [](const Item& a, const Item& b) { return sched::frame_priority(a.frame) < sched::frame_priority(b.frame); });
  _in_flight = std::move(*best);
  _queue.erase(best);
  auto& item = *_in_flight;
  if (item.reply)
    if (const auto found = _inbound.find(item.request_id); found != _inbound.end()) item.frame = more_urgent(found->second, item.frame);
  const auto& payload = item.message.payload;
  const size_t remaining = payload.size() - item.offset;
  item.written_now = std::min(remaining, max_frame);
  Fragment out;
  out.request_id = item.request_id;
  out.frame = item.frame;
  out.type = item.message.type;
  out.first = item.offset == 0;
  out.last = remaining <= max_frame;
  out.payload = std::span<const uint8_t>(payload).subspan(item.offset, item.written_now);
  return out;
}

void OutboundQueue::written() {
  if (!_in_flight) return;
  auto item = std::move(*_in_flight);
  _in_flight.reset();
  item.offset += item.written_now;
  const bool done = item.offset >= item.message.payload.size();
  if (!done) {
    const bool cancelled = item.reply ? _cancelled_inbound.contains(item.request_id) : _cancelled_outgoing.erase(item.request_id) > 0;
    if (cancelled) {
      if (item.reply) {
        _cancelled_inbound.erase(item.request_id);
        _inbound.erase(item.request_id);
      } else {
        _outgoing.erase(item.request_id);
      }
      _bytes -= item.message.payload.size();
      return;
    }
    const auto& classes = item.reply ? _inbound : _outgoing;
    if (const auto found = classes.find(item.request_id); found != classes.end()) item.frame = more_urgent(found->second, item.frame);
    _queue.push_back(std::move(item));
    return;
  }
  _bytes -= item.message.payload.size();
  if (item.reply) {
    _inbound.erase(item.request_id);
    _cancelled_inbound.erase(item.request_id);
  } else if (item.request_id) {
    _outgoing.erase(item.request_id);
    _cancelled_outgoing.erase(item.request_id);
  }
  finish(item, true);
}

void OutboundQueue::finish(Item& item, bool sent) {
  if (item.sent) {
    auto told = std::move(item.sent);
    item.sent = {};
    told(sent);
  }
}

void OutboundQueue::abandon() {
  if (_in_flight) {
    finish(*_in_flight, false);
    _in_flight.reset();
  }
  for (auto& item : _queue) finish(item, false);
  _queue.clear();
  _bytes = 0;
  _outgoing.clear();
  _cancelled_outgoing.clear();
}

}  // namespace dannan::net
