//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/agree/wire.h"

#include <stdexcept>

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

void put(Writer& out, const std::optional<ReplicaSet>& set) {
  out.u8(set ? 1 : 0);
  if (set) put(out, *set);
}

std::optional<ReplicaSet> get_optional_set(Reader& in, const char* what) {
  const auto present = in.u8();
  if (present > 1) throw DecodeError(std::string(what) + "'s presence is 0 or 1");
  if (!present) return std::nullopt;
  return get_set(in);
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

Slot get_slot(Reader& in) {
  const auto slot = in.u32();
  if (slot >= max_slots) throw DecodeError("slot " + std::to_string(slot) + " is past the largest table");
  return slot;
}

void put_names(Writer& out, const std::vector<Name>& names) {
  out.u32(static_cast<uint32_t>(names.size()));
  for (const auto& name : names) out.bytes(name);
}

// None is allowed: a transfer's prepare and founding name no register.
std::vector<Name> get_names(Reader& in) {
  const auto count = in.u32();
  if (count > max_names) throw DecodeError("a message naming " + std::to_string(count) + " registers");
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
  put(out, refusal->serving);
  put(out, refusal->pending_from);
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
  out.serving = get_optional_set(in, "a serving set");
  out.pending_from = get_optional_set(in, "a pending set");
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
  out.u64(message.table);
  put(out, message.prepare.set);
  put(out, message.prepare.to);
  out.u32(message.prepare.slot);
  put(out, message.prepare.ballot);
  put_names(out, message.prepare.names);
  out.u8(message.prepare.values ? 1 : 0);
  return out.take();
}

net::Bytes encode(const AcceptMessage& message) {
  Writer out;
  out.u8(message_version);
  out.string(message.space);
  out.u64(message.table);
  put(out, message.accept.set);
  put(out, message.accept.to);
  out.u32(message.accept.slot);
  put(out, message.accept.ballot);
  put_names(out, message.accept.names);
  for (const auto& value : message.accept.values) put(out, value);
  for (const auto& changed : message.accept.changed_at) put(out, changed);
  out.u8(message.accept.fast_from ? 1 : 0);
  if (message.accept.fast_from) put(out, *message.accept.fast_from);
  out.u8(message.accept.reserve ? 1 : 0);
  if (message.accept.reserve) put(out, *message.accept.reserve);
  return out.take();
}

net::Bytes encode(const PurgeMessage& message) {
  Writer out;
  out.u8(message_version);
  out.string(message.space);
  out.u64(message.table);
  put(out, message.purge.set);
  out.u32(message.purge.slot);
  put(out, message.purge.ballot);
  put_names(out, message.purge.names);
  return out.take();
}

net::Bytes encode(const SettleMessage& message) {
  Writer out;
  out.u8(message_version);
  out.string(message.space);
  out.u64(message.table);
  put(out, message.settle.from);
  put(out, message.settle.to);
  out.u32(message.settle.slot);
  put(out, message.settle.ballot);
  return out.take();
}

PurgeMessage decode_purge(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  PurgeMessage out;
  out.space = in.string(max_space);
  out.table = in.u64();
  out.purge.set = get_set(in);
  out.purge.slot = get_slot(in);
  out.purge.ballot = get_ballot(in);
  out.purge.names = get_names(in);
  in.finish();
  return out;
}

SettleMessage decode_settle(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  SettleMessage out;
  out.space = in.string(max_space);
  out.table = in.u64();
  out.settle.from = get_set(in);
  out.settle.to = get_set(in);
  out.settle.slot = get_slot(in);
  out.settle.ballot = get_ballot(in);
  in.finish();
  return out;
}

net::Bytes encode(const PrepareAnswer& answer) {
  Writer out;
  out.u8(message_version);
  put(out, answer.refused);
  put(out, answer.served);
  put(out, answer.pending_from);
  put_names(out, answer.names);
  out.u32(static_cast<uint32_t>(answer.states.size()));
  for (const auto& state : answer.states) {
    put(out, state.accepted);
    put(out, state.value);
    put(out, state.changed_at);
    if (state.summary.size() > max_summary) throw std::invalid_argument("a summary is at most 64 bytes");
    out.bytes(state.summary);
    out.u8(state.has_value || state.value ? 1 : 0);
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
  out.table = in.u64();
  out.prepare.set = get_set(in);
  out.prepare.to = get_optional_set(in, "a transfer's set");
  out.prepare.slot = get_slot(in);
  out.prepare.ballot = get_ballot(in);
  out.prepare.names = get_names(in);
  // A transfer is answered with every name held, so it names none itself.
  if (out.prepare.to && !out.prepare.names.empty()) throw DecodeError("a transfer's prepare names registers");
  const auto values = in.u8();
  if (values > 1) throw DecodeError("a prepare's values wanted is 0 or 1");
  out.prepare.values = values == 1;
  in.finish();
  return out;
}

AcceptMessage decode_accept(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  AcceptMessage out;
  out.space = in.string(max_space);
  out.table = in.u64();
  out.accept.set = get_set(in);
  out.accept.to = get_optional_set(in, "a transfer's set");
  out.accept.slot = get_slot(in);
  out.accept.ballot = get_ballot(in);
  out.accept.names = get_names(in);
  for (size_t i = 0; i < out.accept.names.size(); ++i) out.accept.values.push_back(get_value(in));
  for (size_t i = 0; i < out.accept.names.size(); ++i) out.accept.changed_at.push_back(get_ballot(in));
  const auto fast = in.u8();
  if (fast > 1) throw DecodeError("a fast path's presence is 0 or 1");
  if (fast) out.accept.fast_from = get_ballot(in);
  const auto reserved = in.u8();
  if (reserved > 1) throw DecodeError("a reservation's presence is 0 or 1");
  if (reserved) out.accept.reserve = get_ballot(in);
  // The fast path is a plain change's alone.
  if ((out.accept.fast_from || out.accept.reserve) && out.accept.to) throw DecodeError("a transfer on the fast path");
  in.finish();
  return out;
}

PrepareAnswer decode_prepare_answer(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  PrepareAnswer out;
  out.refused = get_refusal(in);
  out.served = get_optional_set(in, "a served set");
  out.pending_from = get_optional_set(in, "a pending set");
  out.names = get_names(in);
  const auto count = in.u32();
  if (count > max_names) throw DecodeError("an answer naming " + std::to_string(count) + " registers");
  for (uint32_t i = 0; i < count; ++i) {
    NameState state;
    state.accepted = get_ballot(in);
    state.value = get_value(in);
    state.changed_at = get_ballot(in);
    state.summary = in.bytes(max_summary);
    const auto held = in.u8();
    if (held > 1) throw DecodeError("a name's has-value is 0 or 1");
    state.has_value = held == 1;
    if (state.value && !state.has_value) throw DecodeError("a value carried for a name that holds none");
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

net::Bytes encode(const FetchMessage& message) {
  Writer out;
  out.u8(message_version);
  out.string(message.space);
  out.u64(message.table);
  put(out, message.set);
  out.u32(message.slot);
  out.bytes(message.name);
  put(out, message.ballot);
  out.u64(message.offset);
  if (message.since && message.since->size() > max_summary) throw std::invalid_argument("a fetch's since is a summary, at most 64 bytes");
  put(out, message.since);
  return out.take();
}

net::Bytes encode(const FetchAnswer& answer) {
  if (answer.piece.size() > max_piece) throw std::invalid_argument("a fetched piece is at most 1 MiB");
  Writer out;
  out.u8(message_version);
  put(out, answer.refused);
  out.u8(answer.held ? 1 : 0);
  out.u64(answer.total);
  out.u64(answer.offset);
  out.bytes(answer.piece);
  return out.take();
}

FetchMessage decode_fetch(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  FetchMessage out;
  out.space = in.string(max_space);
  out.table = in.u64();
  out.set = get_set(in);
  out.slot = get_slot(in);
  out.name = in.bytes(max_name);
  out.ballot = get_ballot(in);
  out.offset = in.u64();
  if (out.offset > max_value) throw DecodeError("a fetch's offset is past the largest value");
  const auto since = in.u8();
  if (since > 1) throw DecodeError("a fetch's since's presence is 0 or 1");
  if (since) out.since = in.bytes(max_summary);
  in.finish();
  return out;
}

FetchAnswer decode_fetch_answer(std::span<const uint8_t> bytes) {
  Reader in(bytes);
  open(in);
  FetchAnswer out;
  out.refused = get_refusal(in);
  const auto held = in.u8();
  if (held > 1) throw DecodeError("a fetch's held is 0 or 1");
  out.held = held == 1;
  out.total = in.u64();
  out.offset = in.u64();
  out.piece = in.bytes(max_piece);
  in.finish();
  if (out.total > max_value) throw DecodeError("a fetched value over the largest value");
  if (!out.held && (out.total || out.offset || !out.piece.empty())) throw DecodeError("a fetch not held carries a piece");
  if (out.offset > out.total || out.piece.size() > out.total - out.offset) throw DecodeError("a fetched piece outside its value");
  return out;
}

uint64_t table_fingerprint(uint32_t slots, const ReplicaSet& genesis) {
  Writer out;
  out.u32(slots);
  out.u64(genesis.version);
  out.u16(static_cast<uint16_t>(genesis.nodes.size()));
  for (const auto& node : genesis.nodes) out.fixed(node.bytes);
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const auto byte : out.take()) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

std::array<uint8_t, 40> encode_ballot(const Ballot& ballot) noexcept {
  std::array<uint8_t, 40> out{};
  for (int i = 0; i < 8; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(ballot.counter >> (56 - 8 * i));
  std::copy(ballot.node.bytes.begin(), ballot.node.bytes.end(), out.begin() + 8);
  std::copy(ballot.epoch.bytes.begin(), ballot.epoch.bytes.end(), out.begin() + 24);
  return out;
}

}  // namespace dannan::agree
