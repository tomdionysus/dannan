//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/assembler.h"

#include <stdexcept>

namespace dannan::net {

MessageAssembler::MessageAssembler(size_t max_incomplete_messages, size_t max_incomplete_bytes, size_t max_message_bytes, sched::RetainedMemoryLedger* memory)
    : _max_messages(max_incomplete_messages), _max_bytes(max_incomplete_bytes), _max_message(max_message_bytes), _memory(memory) {
  if (!_max_messages || !_max_bytes || !_max_message) throw std::invalid_argument("reassembly bounds must be nonzero");
}

void MessageAssembler::charge(MemoryHold& memory, sched::FrameType frame, size_t bytes) {
  if (!_memory) return;
  if (!memory) memory = std::make_shared<std::vector<sched::RetainedMemoryLedger::Lease>>();
  auto lease = _memory->try_acquire(sched::work_class(frame), sched::MemoryOwner::rpc_frame, bytes + sizeof(sched::RetainedMemoryLedger::Lease));
  if (!lease) throw FrameRefused("process retained-memory reassembly saturated");
  memory->push_back(std::move(*lease));
}

std::optional<Frame> MessageAssembler::push(WireFragment fragment) {
  if (fragment.request_id == 0) {
    if (!fragment.first || !fragment.last) throw FrameRefused("a notification must fit one frame");
    MemoryHold memory;
    charge(memory, fragment.frame, fragment.payload.size());
    return Frame{0, fragment.frame, {fragment.type, std::move(fragment.payload), std::move(memory)}};
  }
  auto found = _partial.find(fragment.request_id);
  if (fragment.first) {
    if (found != _partial.end()) throw FrameRefused("duplicate first fragment");
    if (_partial.size() >= _max_messages) throw FrameRefused("too many incomplete messages");
    found = _partial.emplace(fragment.request_id, Partial{fragment.frame, fragment.type, {}, {}}).first;
  } else if (found == _partial.end()) {
    throw FrameRefused("continuation without a first fragment");
  }
  auto& partial = found->second;
  if (partial.type != fragment.type) throw FrameRefused("message type changed during transfer");
  if (sched::frame_priority(fragment.frame) > sched::frame_priority(partial.frame)) throw FrameRefused("frame class was demoted during transfer");
  partial.frame = more_urgent(fragment.frame, partial.frame);
  if (partial.payload.size() > _max_message || fragment.payload.size() > _max_message - partial.payload.size()) throw FrameRefused("message too large");
  if (_partial_bytes > _max_bytes || fragment.payload.size() > _max_bytes - _partial_bytes) throw FrameRefused("incomplete reassembly budget exceeded");
  charge(partial.memory, partial.frame, fragment.payload.size());
  partial.payload.insert(partial.payload.end(), fragment.payload.begin(), fragment.payload.end());
  _partial_bytes += fragment.payload.size();
  if (!fragment.last) return std::nullopt;
  Frame out{fragment.request_id, partial.frame, {partial.type, std::move(partial.payload), std::move(partial.memory)}};
  _partial_bytes -= out.message.payload.size();
  _partial.erase(found);
  return out;
}

void MessageAssembler::promote(uint64_t request_id, sched::FrameType frame) {
  const auto found = _partial.find(request_id);
  if (found != _partial.end()) found->second.frame = more_urgent(frame, found->second.frame);
}

void MessageAssembler::discard(uint64_t request_id) {
  const auto found = _partial.find(request_id);
  if (found == _partial.end()) return;
  _partial_bytes -= found->second.payload.size();
  _partial.erase(found);
}

void MessageAssembler::clear() {
  _partial.clear();
  _partial_bytes = 0;
}

}  // namespace dannan::net
