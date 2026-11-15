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

// Agreement's messages on Dannan's wire (an addition to wire version 2): a prepare or an
// accept for one space, and their answers. Every length is bounded before anything is
// allocated; a decode that cannot complete throws net::DecodeError, nothing else.
namespace dannan::agree {

// Bounds a peer's message is held to.
inline constexpr size_t max_space = 255;
inline constexpr size_t max_replicas = 64;
inline constexpr size_t max_names = 4096;
inline constexpr size_t max_name = 64 * 1024;
inline constexpr size_t max_value = 256u * 1024 * 1024;
inline constexpr size_t max_why = 1024;

struct PrepareMessage {
  std::string space;
  Prepare prepare;
};
struct AcceptMessage {
  std::string space;
  Accept accept;
};

net::Bytes encode(const PrepareMessage& message);
net::Bytes encode(const AcceptMessage& message);
net::Bytes encode(const PrepareAnswer& answer);
net::Bytes encode(const AcceptAnswer& answer);

PrepareMessage decode_prepare(std::span<const uint8_t> bytes);
AcceptMessage decode_accept(std::span<const uint8_t> bytes);
PrepareAnswer decode_prepare_answer(std::span<const uint8_t> bytes);
AcceptAnswer decode_accept_answer(std::span<const uint8_t> bytes);

// A ballot as a third party compares it (Louhi's fencing token): the counter, then the
// node id, then the epoch, 40 bytes big-endian, so byte order is ballot order.
std::array<uint8_t, 40> encode_ballot(const Ballot& ballot) noexcept;

}  // namespace dannan::agree
