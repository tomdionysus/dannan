//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/agree/wire.h"

namespace dannan::agree {
namespace {

// Every message begins with its own version, so a later one can be refused by name.
constexpr uint8_t message_version = 1;

using net::DecodeError;
using net::Reader;
using net::Writer;

void put(Writer& out, const Ballot& ballot) {
  out.u64(ballot.counter);
  out.fixed(ballot.node.bytes);
  out.fixed(ballot.epoch.bytes);
}

Ballot get_ballot(Reader& in) {
  Ballot out;
  out.counter = in.u64();
  out.node.bytes = in.fixed<16>();
  out.epoch.bytes = in.fixed<16>();
  return out;
}

void put(Writer& out, const ReplicaSet& set) {
  out.u64(set.version);
  out.u16(static_cast<uint16_t>(set.nodes.size()));
  for (const auto& node : set.nodes) out.fixed(node.bytes);
}

ReplicaSet get_set(Reader& in) {
  ReplicaSet out;
  out.version = in.u64();
  const auto count = in.u16();
  if (count == 0 || count > max_replicas) throw DecodeError("a replica set of " + std::to_string(count) + " nodes");
  for (uint16_t i = 0; i < count; ++i) {
    identity::NodeId node;
    node.bytes = in.fixed<16>();
    // Sorted and unique as made, so no decoder accepts two spellings of one set.
    if (!out.nodes.empty() && !(out.nodes.back() < node)) throw DecodeError("a replica set's nodes out of order");
    out.nodes.push_back(node);
  }
  return out;
}

void put(Writer& out, const std::optional<Bytes>& value) {
  out.u8(value ? 1 : 0);
  if (value) out.bytes(*value);
}

std::optional<Bytes> get_value(Reader& in) {
  const auto present = in.u8();
  if (present > 1) throw DecodeError("a value's presence is 0 or 1");
  if (!present) return std::nullopt;
  return in.bytes(max_value);
}

void put_names(Writer& out, const std::vector<Name>& names) {
  out.u32(static_cast<uint32_t>(names.size()));
  for (const auto& name : names) out.bytes(name);
}

std::vector<Name> get_names(Reader& in) {
  const auto count = in.u32();
  if (count == 0 || count > max_names) throw DecodeError("a request naming " + std::to_string(count) + " registers");
  std::vector<Name> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) out.push_back(in.bytes(max_name));
  return out;
}

void put(Writer& out, const std::optional<Refusal>& refusal) {
  out.u8(refusal ? 1 : 0);
  if (!refusal) return;
  out.string(refusal->why);
  out.u8(refusal->higher ? 1 : 0);
  if (refusal->higher) put(out, *refusal->higher);
  out.u8(refusal->serving ? 1 : 0);
  if (refusal->serving) put(out, *refusal->serving);
}

std::optional<Refusal> get_refusal(Reader& in) {
  const auto present = in.u8();
  if (present > 1) throw DecodeError("a refusal's presence is 0 or 1");
  if (!present) return std::nullopt;
  Refusal out;
  out.why = in.string(max_why);
  const auto higher = in.u8();
  if (higher > 1) throw DecodeError("a higher ballot's presence is 0 or 1");
  if (higher) out.higher = get_ballot(in);
  const auto serving = in.u8();
  if (serving > 1) throw DecodeError("a serving set's presence is 0 or 1");
  if (serving) out.serving = get_set(in);
  return out;
}

void open(Reader& in) {
  const auto version = in.u8();
  if (version != message_version) throw DecodeError("an agreement message of version " + std::to_string(version));
}

}  // namespace

net::Bytes encode(const PrepareMessage& message) {
  Writer out;
  out.u8(message_version);
  out.string(message.space);
  put(out, message.prepare.set);
  put(out, message.prepare.ballot);
  put_names(out, message.prepare.names);
  return out.take();
}

net::Bytes encode(const AcceptMessage& message) {
  Writer out;
  out.u8(message_version);
  out.string(message.space);
  put(out, message.accept.set);
  put(out, message.accept.ballot);
  put_names(out, message.accept.names);
  for (const auto& value : message.accept.values) put(out, value);
  for (const auto& changed : message.accept.changed_at) put(out, changed);
  return out.take();
}

net::Bytes encode(const PrepareAnswer& answer) {
  Writer out;
  out.u8(message_version);
  put(out, answer.refused);
  out.u32(static_cast<uint32_t>(answer.states.size()));
  for (const auto& state : answer.states) {
    put(out, state.accepted);
    put(out, state.value);
    put(out, state.changed_at);
  }
  return out.take();
}

net::Bytes encode(const AcceptAnswer& answer) {
  Writer out;
  out.u8(message_version);
  put(out, answer.refused);
  return out.take();
}

PrepareMessage decode_prepare(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  PrepareMessage out;
  out.space = in.string(max_space);
  out.prepare.set = get_set(in);
  out.prepare.ballot = get_ballot(in);
  out.prepare.names = get_names(in);
  in.finish();
  return out;
}

AcceptMessage decode_accept(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  AcceptMessage out;
  out.space = in.string(max_space);
  out.accept.set = get_set(in);
  out.accept.ballot = get_ballot(in);
  out.accept.names = get_names(in);
  for (size_t i = 0; i < out.accept.names.size(); ++i) out.accept.values.push_back(get_value(in));
  for (size_t i = 0; i < out.accept.names.size(); ++i) out.accept.changed_at.push_back(get_ballot(in));
  in.finish();
  return out;
}

PrepareAnswer decode_prepare_answer(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  PrepareAnswer out;
  out.refused = get_refusal(in);
  const auto count = in.u32();
  if (count > max_names) throw DecodeError("an answer naming " + std::to_string(count) + " registers");
  for (uint32_t i = 0; i < count; ++i) {
    NameState state;
    state.accepted = get_ballot(in);
    state.value = get_value(in);
    state.changed_at = get_ballot(in);
    out.states.push_back(std::move(state));
  }
  in.finish();
  return out;
}

AcceptAnswer decode_accept_answer(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  AcceptAnswer out;
  out.refused = get_refusal(in);
  in.finish();
  return out;
}

std::array<uint8_t, 40> encode_ballot(const Ballot& ballot) noexcept {
  std::array<uint8_t, 40> out{};
  for (int i = 0; i < 8; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(ballot.counter >> (56 - 8 * i));
  std::copy(ballot.node.bytes.begin(), ballot.node.bytes.end(), out.begin() + 8);
  std::copy(ballot.epoch.bytes.begin(), ballot.epoch.bytes.end(), out.begin() + 24);
  return out;
}

}  // namespace dannan::agree
