//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// Message types, lanes and the route table: what each type may do on the wire.

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "dannan/sched/work.h"

namespace dannan::net {

/// A wire message type: a 16-bit number.
///
/// Dannan's core types are below first_user_type; the rest are the hosted systems', each
/// registered at start with its own rules (wire v1, section 4).
enum class MessageType : uint16_t {};

/// The first number a hosted system may register. User types run from here, registered freely
/// (the maintainer, 2026-11-04).
inline constexpr uint16_t first_user_type = 512;

/// The lane a session carries. Each session carries exactly one. Wire values are fixed.
enum class Lane : uint8_t {
  /// Membership, heads, leases, notices and small calls.
  control = 1,
  /// Object payloads.
  data = 2,
  /// Proves only that a handshake can complete; carries nothing.
  probe = 3
};

/// The lane's name for logs and evidence ("control", "data" or "probe"). Never null.
const char* lane_name(Lane) noexcept;

/// Dannan's own message types, with their wire v1 numbers: requests from 1, replies from 100.
///
/// They are listed for logs, tests and tools. A hosted system never sends them itself, and never
/// registers a number below first_user_type.
namespace message {
/// @cond DANNAN_CORE_TYPES
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
// Agreement (0.9.0): a prepare and an accept for one space's registers.
inline constexpr MessageType agree_prepare{22};
inline constexpr MessageType agree_accept{23};
inline constexpr MessageType agree_purge{24};
inline constexpr MessageType agree_settle{25};
// A value-less prepare's base, fetched by its ballot, in pieces.
inline constexpr MessageType agree_fetch{26};
// A clock probe (0.10.0): the sender's wall, answered with the answerer's (membership/clock_offsets.h).
inline constexpr MessageType clock_probe{27};
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
inline constexpr MessageType agree_prepare_reply{110};
inline constexpr MessageType agree_accept_reply{111};
inline constexpr MessageType agree_purge_reply{112};
inline constexpr MessageType agree_settle_reply{113};
inline constexpr MessageType agree_fetch_reply{114};
inline constexpr MessageType clock_probe_reply{115};
inline constexpr MessageType error{127};
/// @endcond
}  // namespace message

/// Which lanes a type may travel on.
enum class Lanes : uint8_t {
  /// The control lane only.
  control,
  /// The data lane only.
  data,
  /// Either lane.
  both
};

/// Which frame classes a type may travel in.
enum class Frames : uint8_t {
  /// Only the control frame.
  control_only,
  /// Any class: data work, and replies, which inherit their request's class.
  any,
  /// Any class but control: bulk objects.
  not_control
};

/// A type's rules on the wire, given once when it is registered.
///
/// @code
/// net::Registration put;
/// put.name = "put";
/// put.lanes = net::Lanes::data;
/// put.frames = net::Frames::not_control;
/// put.ordered = true;
/// @endcode
struct Registration {
  /// The type's name in logs and evidence.
  std::string name;
  /// The lanes it may travel on.
  Lanes lanes = Lanes::control;
  /// The frame classes it may travel in.
  Frames frames = Frames::control_only;
  /// Sent with request id 0 and no reply expected. Such a frame is dispatched only if its type is
  /// registered as a notification, and discarded otherwise.
  bool notification = false;
  /// Answered, in the control frame, by the server's fast pool, which never queues behind
  /// storage-backed control work. For health and membership.
  bool fast_control = false;
  /// Executed one job per peer at a time, in arrival order, by the server's ordered pool. For
  /// mutations whose order matters.
  bool ordered = false;
  /// This type's own no-progress deadline, in place of its lane's. Zero turns it off; unset, the
  /// lane's applies.
  std::optional<std::chrono::milliseconds> no_progress = std::nullopt;
  /// This type's own stall notice, in place of its lane's. Zero turns it off; unset, the lane's
  /// applies.
  std::optional<std::chrono::milliseconds> stall_notice = std::nullopt;
  /// A notification handled on its arrival, on the session's strand, in arrival order and ahead
  /// of every queue. It is never queued, so never refused. Its handler must not wait.
  bool on_session = false;
};

/// What each message type may do on the wire.
///
/// Built once at start, before any session, then only read: a session checks every frame
/// against it. Registration (add(), retire()) is not thread-safe and happens before the node
/// starts; every const member is safe from any thread once it has, and waits on nothing.
class RouteTable {
 public:
  /// A table holding Dannan's core types, which keep Dannan's rules.
  ///
  /// Every number from first_user_type is a user type: registered by a hosted system with its
  /// own rules, or not registered on this node at all.
  static RouteTable with_dannan_types();

  /// Registers a user type with its rules.
  ///
  /// @throws std::invalid_argument for a number below first_user_type, one already registered,
  ///         or one retired.
  void add(MessageType type, Registration registration);

  /// Marks a number as no longer in use, so it is never reused for something else while old peers
  /// may still send it: add() refuses it from then on.
  ///
  /// @throws std::invalid_argument under the same limits as add().
  void retire(MessageType type);

  /// The type's registration, or null for a type nobody registered. The pointer lives as long as
  /// the table.
  const Registration* find(MessageType type) const noexcept;

  /// Whether `type` may travel on `lane` in `frame`.
  ///
  /// A core or registered type is judged by its registration. A type nobody registered here is
  /// not policed (any class, either lane): a user type is the user's to accept or decline, and a
  /// core number this version does not know is a newer peer's, answered as unrecognised. Nothing
  /// travels on the probe lane.
  bool allowed(MessageType type, Lane lane, sched::FrameType frame) const noexcept;

  /// Whether the number is below first_user_type: Dannan's, known to this version or not.
  bool is_core(MessageType type) const noexcept;
  /// Whether the type is registered as a notification.
  bool is_notification(MessageType type) const noexcept;
  /// Whether the type is answered by the fast pool.
  bool is_fast_control(MessageType type) const noexcept;
  /// Whether the type is executed in order per peer.
  bool is_ordered(MessageType type) const noexcept;
  /// Whether the type is a notification handled on the session's strand.
  bool is_on_session(MessageType type) const noexcept;

  /// The registered name, or the number in decimal for a type nobody registered.
  std::string name(MessageType type) const;

  /// @cond PRIVATE
 private:
  void add_core(MessageType type, Registration registration);

  std::map<MessageType, Registration> _types;
  std::set<MessageType> _retired;
  /// @endcond
};

}  // namespace dannan::net
