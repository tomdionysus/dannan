//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/work.h"

#include <stdexcept>

namespace dannan::sched {

std::optional<FrameType> decode_frame_type(uint8_t value) noexcept {
  if (value >= 1 && value <= 5) return static_cast<FrameType>(value);
  return std::nullopt;
}

const char* frame_type_name(FrameType type) noexcept {
  switch (type) {
    case FrameType::control:
      return "control";
    case FrameType::foreground:
      return "foreground";
    case FrameType::read_ahead:
      return "read-ahead";
    case FrameType::loader:
      return "loader";
    case FrameType::speculative:
      return "speculative";
  }
  return "unknown";
}

const char* work_class_name(WorkClass work) noexcept {
  switch (work) {
    case WorkClass::control:
      return "control";
    case WorkClass::foreground:
      return "foreground";
    case WorkClass::loader:
      return "loader";
    case WorkClass::speculative:
      return "speculative";
  }
  return "unknown";
}

bool WaitGuard::enter(const WorkContext& context, Waits declared, std::string_view operation) {
  if (may_enter(context.allowed(), declared)) return true;
  _violations.fetch_add(1, std::memory_order_relaxed);
  const std::string message =
      std::string("work (") + context.origin() + ") entered " + std::string(operation) + ", which may wait on what the work does not allow";
  if (mode() == Mode::throw_on_violation) throw std::logic_error(message);
  bool first = false;
  {
    contract::Lock lock(_mutex);
    first = _reported.emplace(std::string(context.origin()) + " -> " + std::string(operation)).second;
  }
  if (first) _logger.warn(message);
  return false;
}

}  // namespace dannan::sched
