//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/routes.h"

#include <array>
#include <stdexcept>

namespace dannan::net {
namespace {

// Never reused: protocol 23 retired them.
constexpr std::array<uint16_t, 4> retired{37, 38, 48, 124};

}  // namespace

const char* lane_name(Lane lane) noexcept {
  switch (lane) {
    case Lane::control:
      return "control";
    case Lane::data:
      return "data";
    case Lane::probe:
      return "probe";
  }
  return "unknown";
}

RouteTable RouteTable::with_dannan_types() {
  RouteTable table;
  using enum Lanes;
  using enum Frames;
  // ping rides both lanes, so a DATA lane's dead NAT mapping is noticed.
  table.add(message::ping, {"ping", both, control_only, false, true});
  table.add(message::members, {"members", control, control_only, false, true});
  // A session's own transport controls, on either lane.
  table.add(message::session_retire, {"session_retire", both, control_only, false});
  table.add(message::promote_read_ahead, {"promote_read_ahead", both, control_only, false});
  table.add(message::promote_foreground, {"promote_foreground", both, control_only, false});
  table.add(message::cancel_transfer, {"cancel_transfer", both, control_only, false});
  // Gossip: speculative work, or control.
  table.add(message::telemetry, {"telemetry", control, any, true});
  table.add(message::identity_resets, {"identity_resets", control, control_only, false});
  table.add(message::dial_request, {"dial_request", control, control_only, true});
  table.add(message::dial_back_probe, {"dial_back_probe", control, control_only, false});
  // Replies inherit their request's class.
  table.add(message::ok, {"ok", both, any, false});
  table.add(message::error, {"error", both, any, false});
  table.add(message::members_reply, {"members_reply", control, control_only, false});
  table.add(message::bool_reply, {"bool_reply", control, any, false});
  table.add(message::telemetry_reply, {"telemetry_reply", control, any, false});
  table.add(message::identity_resets_reply, {"identity_resets_reply", control, control_only, false});
  table.add(message::dial_back_probe_reply, {"dial_back_probe_reply", control, control_only, false});
  for (const auto& [type, _] : table._types) table._core.insert(type);
  return table;
}

void RouteTable::add(MessageType type, Registration registration) {
  const auto number = static_cast<uint16_t>(type);
  for (const auto old : retired)
    if (number == old) throw std::invalid_argument("message type " + std::to_string(number) + " is retired and never reused");
  if (_types.contains(type)) throw std::invalid_argument("message type " + std::to_string(number) + " is already registered as " + _types.at(type).name);
  _types.emplace(type, std::move(registration));
}

const Registration* RouteTable::find(MessageType type) const noexcept {
  const auto found = _types.find(type);
  return found == _types.end() ? nullptr : &found->second;
}

bool RouteTable::allowed(MessageType type, Lane lane, sched::FrameType frame) const noexcept {
  if (lane == Lane::probe) return false;
  const auto* registration = find(type);
  if (!registration) return true;
  const auto lanes = registration->lanes;
  const auto frames = registration->frames;
  const bool lane_ok = lanes == Lanes::both || (lanes == Lanes::control && lane == Lane::control) || (lanes == Lanes::data && lane == Lane::data);
  if (!lane_ok) return false;
  switch (frames) {
    case Frames::control_only:
      return frame == sched::FrameType::control;
    case Frames::any:
      return true;
    case Frames::not_control:
      return frame != sched::FrameType::control;
  }
  return false;
}

bool RouteTable::is_core(MessageType type) const noexcept { return _core.contains(type); }

bool RouteTable::is_notification(MessageType type) const noexcept {
  const auto* registration = find(type);
  return registration && registration->notification;
}

bool RouteTable::is_fast_control(MessageType type) const noexcept {
  const auto* registration = find(type);
  return registration && registration->fast_control;
}

bool RouteTable::is_ordered(MessageType type) const noexcept {
  const auto* registration = find(type);
  return registration && registration->ordered;
}

std::string RouteTable::name(MessageType type) const {
  const auto* registration = find(type);
  return registration ? registration->name : std::to_string(static_cast<uint16_t>(type));
}

}  // namespace dannan::net
