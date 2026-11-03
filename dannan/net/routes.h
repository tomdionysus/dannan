//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "dannan/sched/work.h"

namespace dannan::net {

// A wire message type: a 16-bit number. The space is open: Dannan reserves
// its own numbers, and every hosted system registers its own at start. The
// numbers protocol 23 uses never move.
enum class MessageType : uint16_t {};

// A session carries one lane: CONTROL for membership, heads, leases, notices
// and small calls; DATA for object payloads; or a probe, which only proves a
// handshake can complete and carries nothing. Wire values are fixed.
enum class Lane : uint8_t { control = 1, data = 2, probe = 3 };
const char* lane_name(Lane) noexcept;

// Dannan's own types: the transport's, membership's, telemetry's and
// reachability's, with their protocol-23 numbers.
namespace message {
inline constexpr MessageType ping{1};
inline constexpr MessageType members{2};
inline constexpr MessageType session_retire{12};
inline constexpr MessageType promote_read_ahead{14};
inline constexpr MessageType promote_foreground{15};
inline constexpr MessageType cancel_transfer{16};
inline constexpr MessageType telemetry{24};
inline constexpr MessageType identity_resets{25};
inline constexpr MessageType dial_request{43};
inline constexpr MessageType dial_back_probe{44};
inline constexpr MessageType ok{100};
inline constexpr MessageType error{101};
inline constexpr MessageType members_reply{102};
inline constexpr MessageType bool_reply{103};
inline constexpr MessageType telemetry_reply{109};
inline constexpr MessageType identity_resets_reply{110};
inline constexpr MessageType dial_back_probe_reply{120};
}  // namespace message

// Which lanes a type may travel on.
enum class Lanes : uint8_t { control, data, both };

// Which frame classes a type may travel in: only the control frame; any
// class (data work, and replies, which inherit their request's class); or
// any but control (bulk objects).
enum class Frames : uint8_t { control_only, any, not_control };

struct Registration {
  std::string name;
  Lanes lanes = Lanes::control;
  Frames frames = Frames::control_only;
  // Sent with request id 0, no reply expected; dispatched only if so
  // registered, discarded otherwise.
  bool notification = false;
  // Answered, in the control frame, by the server's fast pool, which never
  // queues behind storage-backed control work (health, membership).
  bool fast_control = false;
  // Executed one job per peer at a time, in arrival order, by the server's
  // ordered pool: mutations whose order matters.
  bool ordered = false;
};

// What each type may do on the wire. Built once at start, before any
// session, then only read: a session checks every frame against it.
class RouteTable {
 public:
  // A table holding Dannan's core types, which keep Dannan's rules. Every
  // other number is a user type, registered by a hosted system with its own
  // rules, or not registered on this node at all.
  static RouteTable with_dannan_types();

  // Registers `type`. Throws std::invalid_argument if the number is taken or
  // retired. Not thread-safe: registration happens before the node starts.
  void add(MessageType type, Registration registration);

  // The registration, or null for a type nobody registered.
  const Registration* find(MessageType type) const noexcept;
  // Whether `type` may travel on `lane` in `frame`: a core or registered
  // type by its registration; a user type nobody registered here is not
  // policed (any class, either lane), since it is the user's to accept or
  // decline. Nothing travels on the probe lane.
  bool allowed(MessageType type, Lane lane, sched::FrameType frame) const noexcept;
  // One of Dannan's own types.
  bool is_core(MessageType type) const noexcept;
  bool is_notification(MessageType type) const noexcept;
  bool is_fast_control(MessageType type) const noexcept;
  bool is_ordered(MessageType type) const noexcept;
  // The registered name, or the number in decimal.
  std::string name(MessageType type) const;

 private:
  std::map<MessageType, Registration> _types;
  std::set<MessageType> _core;
};

}  // namespace dannan::net
