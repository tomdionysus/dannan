//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/agree/memory_store.h"

#include <algorithm>
#include <boost/asio/post.hpp>

#include "dannan/identity/crypto.h"

namespace dannan::agree {

MemoryStore::MemoryStore(boost::asio::any_io_executor executor, bool ready, Codec codec)
    : _executor(std::move(executor)), _ready(ready), _codec(std::move(codec)) {}

Bytes MemoryStore::summary_of(const Bytes& value) const {
  if (_codec.summary) return _codec.summary(value);
  const auto digest = identity::sha256(value);
  return Bytes(digest.bytes.begin(), digest.bytes.end());
}

void MemoryStore::load(Slot slot, std::span<const Name> names, bool every, bool values, std::function<void(Held)> done) {
  Held out;
  {
    contract::Lock lock(_mutex);
    out.floor = _floor;
    const auto found = _slots.find(slot);
    if (found != _slots.end()) out.slot = found->second.record;
    for (const auto& name : names) {
      out.names.push_back(name);
      AcceptorState state;
      if (found != _slots.end()) {
        const auto held = found->second.names.find(key_of(name));
        if (held != found->second.names.end()) state = held->second;
      }
      out.states.push_back(std::move(state));
    }
    if (every && found != _slots.end())
      for (const auto& [key, state] : found->second.names) {
        const Name name(key.begin(), key.end());
        if (std::find(names.begin(), names.end(), name) != names.end()) continue;
        out.names.push_back(name);
        out.states.push_back(state);
      }
  }
  if (!values)
    for (auto& state : out.states) state.value.reset();
  boost::asio::post(_executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
}

std::optional<std::vector<AcceptorState>> MemoryStore::prepared(Slot slot, const Held& held, Kept& refusal) const {
  const auto found = _slots.find(slot);
  // Taken here, under the lock prepared() requires: the analysis does not carry it into a lambda.
  const bool none = found == _slots.end();
  const auto current = [&](size_t i) -> const AcceptorState* {
    if (none) return nullptr;
    const auto it = found->second.names.find(key_of(held.names[i]));
    return it == found->second.names.end() ? nullptr : &it->second;
  };
  std::vector<AcceptorState> out;
  out.reserve(held.states.size());
  bool too_long = false;
  for (size_t i = 0; i < held.states.size(); ++i) {
    auto state = held.states[i];
    if (state == AcceptorState{}) {
      out.push_back(std::move(state));
      continue;
    }
    const auto* before = current(i);
    if (state.delta) {
      const auto at = before ? before->accepted : Ballot{};
      std::optional<Bytes> applied;
      if (at == state.base && _codec.apply) applied = _codec.apply(before && before->value ? *before->value : Bytes{}, *state.delta);
      if (!applied) {
        refusal.need_whole.push_back(i);
        out.push_back(std::move(state));
        continue;
      }
      state.value = std::move(*applied);
      state.delta.reset();
      state.base = {};
      state.has_value = true;
      state.summary = summary_of(*state.value);
    } else if (state.has_value && !state.value) {
      // Held, not carried: the value and its summary stay as they are.
      if (before) {
        state.value = before->value;
        state.summary = before->summary;
      }
    } else {
      state.has_value = state.value.has_value();
      state.summary = state.value ? summary_of(*state.value) : Bytes{};
    }
    if (state.summary.size() > max_summary) too_long = true;
    out.push_back(std::move(state));
  }
  if (too_long) {
    // Refused whole: no name is asked for again, since sending it whole would not help.
    refusal.need_whole.clear();
    return std::nullopt;
  }
  if (!refusal.need_whole.empty()) return std::nullopt;
  return out;
}

void MemoryStore::keep(Slot slot, const Held& held, std::function<void(Kept)> done) {
  Kept out;
  {
    contract::Lock lock(_mutex);
    if (auto states = prepared(slot, held, out)) {
      _floor = held.floor;
      auto& at = _slots[slot];
      at.record = held.slot;
      for (size_t i = 0; i < held.names.size(); ++i) {
        const auto key = key_of(held.names[i]);
        if ((*states)[i] == AcceptorState{})
          at.names.erase(key);
        else
          at.names[key] = std::move((*states)[i]);
      }
      out.kept = true;
    }
  }
  boost::asio::post(_executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
}

void MemoryStore::records(std::function<void(std::vector<std::pair<Slot, SlotState>>)> done) {
  std::vector<std::pair<Slot, SlotState>> out;
  {
    contract::Lock lock(_mutex);
    for (const auto& [slot, held] : _slots) out.emplace_back(slot, held.record);
  }
  boost::asio::post(_executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
}

std::optional<AcceptorState> MemoryStore::state(Slot slot, const Name& name) const {
  contract::Lock lock(_mutex);
  const auto found = _slots.find(slot);
  if (found == _slots.end()) return std::nullopt;
  const auto held = found->second.names.find(key_of(name));
  if (held == found->second.names.end()) return std::nullopt;
  return held->second;
}

size_t MemoryStore::size() const {
  contract::Lock lock(_mutex);
  size_t total = 0;
  for (const auto& [slot, held] : _slots) total += held.names.size();
  return total;
}

}  // namespace dannan::agree
