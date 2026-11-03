//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/routes.h"

#include <stdexcept>

namespace dannan::net {
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
  table.add_core(message::ping, {"ping", both, control_only, false, true});
  table.add_core(message::members, {"members", control, control_only, false, true});
  // A session's own transport controls, on either lane.
  table.add_core(message::session_retire, {"session_retire", both, control_only, false});
  table.add_core(message::promote_read_ahead, {"promote_read_ahead", both, control_only, false});
  table.add_core(message::promote_foreground, {"promote_foreground", both, control_only, false});
  table.add_core(message::cancel_transfer, {"cancel_transfer", both, control_only, false});
  // Gossip: speculative work, or control.
  table.add_core(message::telemetry, {"telemetry", control, any, true});
  table.add_core(message::identity_resets, {"identity_resets", control, control_only, false});
  // Reachability (level 0): answered fast, never behind hosted work, since
  // a punch is timed and a relayed session is someone's only path.
  table.add_core(message::dial_request, {"dial_request", control, control_only, true});
  table.add_core(message::dial_back, {"dial_back", control, control_only, false});
  table.add_core(message::punch_request, {"punch_request", control, control_only, false, true});
  table.add_core(message::punch_go, {"punch_go", control, control_only, true, true});
  table.add_core(message::relay_open, {"relay_open", control, control_only, false, true});
  table.add_core(message::relay_offer, {"relay_offer", control, control_only, false, true});
  // A circuit's bytes travel in the class of the frame they carry, on the
  // lane the circuit is for, in order.
  table.add_core(message::relay_data, {"relay_data", both, any, true, false, true});
  table.add_core(message::relay_close, {"relay_close", both, control_only, true, true});
  // Replies inherit their request's class.
  table.add_core(message::ok, {"ok", both, any, false});
  table.add_core(message::members_reply, {"members_reply", control, control_only, false});
  table.add_core(message::telemetry_reply, {"telemetry_reply", control, any, false});
  table.add_core(message::identity_resets_reply, {"identity_resets_reply", control, control_only, false});
  table.add_core(message::dial_back_reply, {"dial_back_reply", control, control_only, false});
  table.add_core(message::punch_reply, {"punch_reply", control, control_only, false});
  table.add_core(message::relay_open_reply, {"relay_open_reply", control, control_only, false});
  table.add_core(message::relay_offer_reply, {"relay_offer_reply", control, control_only, false});
  table.add_core(message::error, {"error", both, any, false});
  return table;
}

void RouteTable::add_core(MessageType type, Registration registration) { _types.emplace(type, std::move(registration)); }

void RouteTable::add(MessageType type, Registration registration) {
  const auto number = static_cast<uint16_t>(type);
  if (number < first_user_type) throw std::invalid_argument("message type " + std::to_string(number) + " is Dannan's; user types start at 1024");
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

bool RouteTable::is_core(MessageType type) const noexcept { return static_cast<uint16_t>(type) < first_user_type; }

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
