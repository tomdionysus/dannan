//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "dannan/sched/work.h"

namespace dannan::net {

// A wire message type: a 16-bit number. Dannan's core types are below
// first_user_type, the rest are the hosted systems', each registered at
// start with its own rules (wire v1, section 4).
enum class MessageType : uint16_t {};
// Tom, 2026-11-04: user types from 512, registered freely.
inline constexpr uint16_t first_user_type = 512;

// A session carries one lane: CONTROL for membership, heads, leases, notices
// and small calls; DATA for object payloads; or a probe, which only proves a
// handshake can complete and carries nothing. Wire values are fixed.
enum class Lane : uint8_t { control = 1, data = 2, probe = 3 };
const char* lane_name(Lane) noexcept;

// Dannan's own types, with their wire v1 numbers: requests from 1,
// replies from 100.
namespace message {
inline constexpr MessageType ping{1};
inline constexpr MessageType members{2};
inline constexpr MessageType session_retire{3};
inline constexpr MessageType promote_read_ahead{4};
inline constexpr MessageType promote_foreground{5};
inline constexpr MessageType cancel_transfer{6};
inline constexpr MessageType telemetry{7};
inline constexpr MessageType identity_resets{8};
inline constexpr MessageType dial_request{9};
inline constexpr MessageType dial_back{10};
inline constexpr MessageType punch_request{11};
inline constexpr MessageType punch_go{12};
inline constexpr MessageType relay_open{13};
inline constexpr MessageType relay_offer{14};
inline constexpr MessageType relay_data{15};
inline constexpr MessageType relay_close{16};
inline constexpr MessageType channel_publish{17};
inline constexpr MessageType channel_replay{18};
inline constexpr MessageType channel_bulk{19};
inline constexpr MessageType revocations{20};
inline constexpr MessageType relay_credit{21};
inline constexpr MessageType ok{100};
inline constexpr MessageType members_reply{101};
inline constexpr MessageType telemetry_reply{102};
inline constexpr MessageType identity_resets_reply{103};
inline constexpr MessageType dial_back_reply{104};
inline constexpr MessageType punch_reply{105};
inline constexpr MessageType relay_open_reply{106};
inline constexpr MessageType relay_offer_reply{107};
inline constexpr MessageType channel_replay_reply{108};
inline constexpr MessageType revocations_reply{109};
inline constexpr MessageType error{127};
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
  // This type's own no-progress deadline and stall notice, in place of its
  // lane's; zero turns one off. Unset, the lane's apply.
  std::optional<std::chrono::milliseconds> no_progress = std::nullopt;
  std::optional<std::chrono::milliseconds> stall_notice = std::nullopt;
  // A notification handled by its arrival, on the session's strand, in the
  // order it arrived and ahead of every queue: never queued, so never
  // refused. Its handler must not wait.
  bool on_session = false;
};

// What each type may do on the wire. Built once at start, before any
// session, then only read: a session checks every frame against it.
class RouteTable {
 public:
  // A table holding Dannan's core types, which keep Dannan's rules. Every
  // number from first_user_type is a user type, registered by a hosted
  // system with its own rules, or not registered on this node at all.
  static RouteTable with_dannan_types();

  // Registers a user type. Throws std::invalid_argument for a number below
  // first_user_type or one already taken. Not thread-safe: registration
  // happens before the node starts.
  void add(MessageType type, Registration registration);
  // A number no longer in use, so it is not reused for something else while old
  // peers may still send it: add() then refuses it. The same limits as add().
  void retire(MessageType type);

  // The registration, or null for a type nobody registered.
  const Registration* find(MessageType type) const noexcept;
  // Whether `type` may travel on `lane` in `frame`: a core or registered
  // type by its registration; a type nobody registered here is not policed
  // (any class, either lane), since a user type is the user's to accept or
  // decline and a core number this version does not know is a newer
  // peer's, answered as unrecognised. Nothing travels on the probe lane.
  bool allowed(MessageType type, Lane lane, sched::FrameType frame) const noexcept;
  // Below first_user_type: Dannan's, known to this version or not.
  bool is_core(MessageType type) const noexcept;
  bool is_notification(MessageType type) const noexcept;
  bool is_fast_control(MessageType type) const noexcept;
  bool is_ordered(MessageType type) const noexcept;
  bool is_on_session(MessageType type) const noexcept;
  // The registered name, or the number in decimal.
  std::string name(MessageType type) const;

 private:
  void add_core(MessageType type, Registration registration);

  std::map<MessageType, Registration> _types;
  std::set<MessageType> _retired;
};

}  // namespace dannan::net
