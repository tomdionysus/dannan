//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <string>

#include "dannan/agree/caspaxos.h"
#include "dannan/net/codec.h"

// Agreement's messages on Dannan's wire (an addition to wire version 2): a prepare, an
// accept, a purge or a settle for one slot of one space, and their answers. Every length is bounded before anything is
// allocated; a decode that cannot complete throws net::DecodeError, nothing else.
namespace dannan::agree {

// Bounds a peer's message is held to.
inline constexpr size_t max_space = 255;
inline constexpr size_t max_replicas = 64;
inline constexpr size_t max_names = 4096;
inline constexpr size_t max_name = 64 * 1024;
inline constexpr size_t max_value = 256u * 1024 * 1024;
inline constexpr size_t max_why = 1024;

// Each request carries the sender's slot table as its fingerprint (table_fingerprint): a
// node configured with another count or genesis set is refused by every acceptor, since the
// rule for an unrecorded slot would differ between them.
struct PrepareMessage {
  std::string space;
  Prepare prepare;
  uint64_t table = 0;
};
struct AcceptMessage {
  std::string space;
  Accept accept;
  uint64_t table = 0;
};
struct PurgeMessage {
  std::string space;
  Purge purge;
  uint64_t table = 0;
};
struct SettleMessage {
  std::string space;
  Settle settle;
  uint64_t table = 0;
};

// FNV-1a 64 over the slot count (4 bytes) and the genesis set as the wire writes it.
uint64_t table_fingerprint(uint32_t slots, const ReplicaSet& genesis);

net::Bytes encode(const PrepareMessage& message);
net::Bytes encode(const AcceptMessage& message);
net::Bytes encode(const PurgeMessage& message);
net::Bytes encode(const SettleMessage& message);
net::Bytes encode(const PrepareAnswer& answer);
net::Bytes encode(const AcceptAnswer& answer);

PrepareMessage decode_prepare(std::span<const uint8_t> bytes);
AcceptMessage decode_accept(std::span<const uint8_t> bytes);
// A purge and a settle are answered as an accept is: refused, or done.
PurgeMessage decode_purge(std::span<const uint8_t> bytes);
SettleMessage decode_settle(std::span<const uint8_t> bytes);
PrepareAnswer decode_prepare_answer(std::span<const uint8_t> bytes);
AcceptAnswer decode_accept_answer(std::span<const uint8_t> bytes);

// A ballot as a third party compares it (Louhi's fencing token): the counter, then the
// node id, then the epoch, 40 bytes big-endian, so byte order is ballot order.
std::array<uint8_t, 40> encode_ballot(const Ballot& ballot) noexcept;

}  // namespace dannan::agree
