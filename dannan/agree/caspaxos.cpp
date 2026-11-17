//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/agree/caspaxos.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace dannan::agree {

ReplicaSet ReplicaSet::of(std::vector<identity::NodeId> nodes, uint64_t version) {
  if (nodes.empty()) throw std::invalid_argument("a replica set has at least one node");
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return {std::move(nodes), version};
}

bool ReplicaSet::contains(const identity::NodeId& node) const noexcept { return std::binary_search(nodes.begin(), nodes.end(), node); }

std::vector<Ballot> descend(const Ballot& ballot, const std::vector<Ballot>& of) {
  std::vector<Ballot> out{ballot};
  if (ballot.zero()) return out;
  for (const auto& each : of) {
    if (out.size() == max_ancestry) break;
    out.push_back(each);
    if (each.zero()) break;
  }
  return out;
}

Slot slot_of(std::span<const uint8_t> name, uint32_t count) {
  if (count == 0 || count > max_slots) throw std::invalid_argument("a slot table holds 1 to 8192 slots");
  uint64_t hash = 0xcbf29ce484222325ull;
  for (const auto byte : name) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return static_cast<Slot>(hash % count);
}

namespace {

Refusal refusal(std::string why) { return Refusal{std::move(why), std::nullopt, std::nullopt, std::nullopt}; }

Refusal served_elsewhere(std::string why, const SlotState& slot) { return Refusal{std::move(why), std::nullopt, slot.served, slot.pending_from}; }

bool in_either(const identity::NodeId& node, const ReplicaSet& set, const std::optional<ReplicaSet>& to) {
  return set.contains(node) || (to && to->contains(node));
}

// Refusals every request meets before its ballot is looked at: who may take part, and
// which set serves the slot.
std::optional<Refusal> admitted(const identity::NodeId& self, const identity::NodeId& from, const ReplicaSet& set, const std::optional<ReplicaSet>& to,
                                const Held& held, size_t names, const ReplicaSet& genesis) {
  if (held.states.size() != held.names.size() || held.names.size() < names) return refusal("the request and the state held name different counts");
  if (!in_either(self, set, to)) return refusal("this node is not in the replica set named");
  if (!in_either(from, set, to)) return refusal("the sender is not in the replica set named");
  const auto& slot = held.slot;
  if (!to) {
    // A record of an older configuration than the request's is a laggard's (it missed a
    // transfer and its settle): it promises, showing what it holds, and the proposer's proof
    // rule decides, as for an acceptor with no record.
    if (slot.served && *slot.served != set && !(slot.served->version < set.version)) return served_elsewhere("the slot is served by another replica set", slot);
    return std::nullopt;
  }
  if (!(to->version > set.version)) return refusal("a transfer's new set has a version above the old one's");
  // A node joining takes whatever a majority of the old set gives it: the old set's
  // members are the ones who vouch for the slot, so only their records are looked at.
  if (!set.contains(self)) return std::nullopt;
  if (!slot.served) {
    if (set != genesis) return refusal("the slot is recorded nowhere here, and only the genesis set moves an unrecorded slot");
    return std::nullopt;
  }
  // From the old set once its own move is settled, or again to the same new set.
  if (*slot.served == set && !slot.pending_from) return std::nullopt;
  if (*slot.served == *to && (!slot.pending_from || *slot.pending_from == set)) return std::nullopt;
  return served_elsewhere("the slot is served by another replica set", slot);
}

// The highest ballot held for the slot, its names and the floor: what a transfer passes.
Ballot highest_in_slot(const Held& held) {
  Ballot highest = std::max(held.floor, held.slot.promised);
  for (const auto& state : held.states) highest = std::max({highest, state.promised, state.accepted});
  return highest;
}

// The same over the request's own names only: what a plain change passes.
Ballot highest_of(const Held& held, size_t names) {
  Ballot highest;
  for (size_t i = 0; i < names; ++i) highest = std::max({highest, held.states[i].promised, held.states[i].accepted});
  return highest;
}

}  // namespace

PrepareStep on_prepare(const identity::NodeId& self, const identity::NodeId& from, const Prepare& request, const Held& held, const ReplicaSet& genesis) {
  PrepareStep out;
  const size_t names = request.to ? 0 : request.names.size();
  if (auto refused = admitted(self, from, request.set, request.to, held, names, genesis)) {
    out.answer.refused = std::move(refused);
    return out;
  }
  // A plain change passes its names' promises, the slot's transfer promise and the floor; a
  // transfer passes everything held in the slot.
  const Ballot highest = request.to ? highest_in_slot(held) : std::max({highest_of(held, names), held.slot.promised, held.floor});
  if (!(request.ballot > highest)) {
    out.answer.refused = Refusal{"a higher ballot is promised", highest, std::nullopt, std::nullopt};
    return out;
  }
  const auto answered = request.to ? held.states.size() : names;
  for (size_t i = 0; i < answered; ++i)
    if (held.states[i].summary.size() > max_summary) {
      out.answer.refused = refusal("the store's summary of a name is over 64 bytes");
      return out;
    }
  const auto answer_of = [&request](const AcceptorState& state) {
    return NameState{state.accepted, request.values ? state.value : std::nullopt, state.changed_at, state.summary, holds_value(state), false, state.ancestry};
  };
  Held keep = held;
  out.answer.served = held.slot.served;
  out.answer.pending_from = held.slot.pending_from;
  if (request.to) {
    keep.slot.promised = request.ballot;
    out.answer.names = held.names;
    for (const auto& state : held.states) out.answer.states.push_back(answer_of(state));
  } else {
    for (size_t i = 0; i < names; ++i) {
      keep.states[i].promised = request.ballot;
      out.answer.states.push_back(answer_of(held.states[i]));
    }
  }
  out.keep = std::move(keep);
  return out;
}

AcceptStep on_accept(const identity::NodeId& self, const identity::NodeId& from, const Accept& request, const Held& held, const ReplicaSet& genesis) {
  AcceptStep out;
  const size_t names = request.names.size();
  if (auto refused = admitted(self, from, request.set, request.to, held, names, genesis)) {
    out.answer.refused = std::move(refused);
    return out;
  }
  const bool compact = !request.deltas.empty() || !request.held.empty();
  if (request.values.size() != names || request.changed_at.size() != names || (!request.deltas.empty() && request.deltas.size() != names) ||
      (!request.held.empty() && request.held.size() != names) || (compact && request.bases.size() != names) ||
      (!request.summaries.empty() && request.summaries.size() != names) || (!request.ancestry.empty() && request.ancestry.size() != names)) {
    out.answer.refused = refusal("the accept names, values and changes differ in count");
    return out;
  }
  // This accept again (resent, duplicated, or forget()'s check that every replica holds its
  // tombstone): every name it carries already accepted at its ballot, the slot recorded as it
  // leaves it. Answered as taken, nothing kept. A promise given since, the reservation the
  // accept itself made included, refuses no accept already taken (the review of 2026-11-17).
  const bool recorded = request.to ? held.slot.served == request.to : held.slot.served == request.set;
  if (names > 0 && recorded &&
      std::all_of(held.states.begin(), held.states.begin() + static_cast<std::ptrdiff_t>(names),
                  [&request](const AcceptorState& state) { return state.accepted == request.ballot; }))
    return out;
  const auto delta_at = [&request](size_t i) { return !request.deltas.empty() && request.deltas[i].has_value(); };
  const auto held_at = [&request](size_t i) { return !request.held.empty() && request.held[i] != 0; };
  for (size_t i = 0; i < names; ++i)
    if ((delta_at(i) && (request.values[i] || held_at(i))) || (held_at(i) && request.values[i])) {
      out.answer.refused = refusal("an accept carries a name in two forms");
      return out;
    }
  // The fast path: nothing may have been prepared since its last ballot, here, for these
  // names or for the slot, and the slot is still the set's, settled.
  if (request.fast_from) {
    const auto& last = *request.fast_from;
    bool untouched = !request.to && held.slot.served == request.set && !held.slot.pending_from && !(held.slot.promised > last) && request.ballot > last &&
                     request.ballot > held.floor;
    // Still the reservation the last commit made, and nothing accepted since.
    for (size_t i = 0; i < names && untouched; ++i) untouched = held.states[i].promised == request.ballot && held.states[i].accepted == last;
    if (!untouched) {
      out.answer.refused = Refusal{"the fast path's last ballot is not the last here", std::max({highest_of(held, names), held.floor, held.slot.promised}),
                                   std::nullopt, std::nullopt};
      return out;
    }
  }
  // Accepted at the ballot promised or above; equal is the same proposer's retry. Never at
  // or below the floor, and a plain change never at or below a transfer's promise. A
  // transfer is refused where anything in the slot, named or not, holds a higher ballot.
  const Ballot highest =
      request.to ? std::max(held.slot.promised, highest_in_slot(Held{held.slot, Ballot{}, held.names, held.states})) : highest_of(held, names);
  const bool below = request.ballot < highest || !(request.ballot > held.floor) || (!request.to && !(request.ballot > held.slot.promised));
  if (below) {
    out.answer.refused = Refusal{"a higher ballot is promised", std::max({highest, held.floor, held.slot.promised}), std::nullopt, std::nullopt};
    return out;
  }
  if (request.reserve && (request.to || !(*request.reserve > request.ballot))) {
    out.answer.refused = refusal("a reservation is a plain accept's, above its ballot");
    return out;
  }
  // A delta needs its base held here exactly; otherwise the name is asked for whole and
  // nothing is kept. A name already at this ballot is this accept again (resent, or replayed):
  // kept already, its delta never applied twice.
  // The ballot only likewise needs the value held here at its base.
  for (size_t i = 0; i < names; ++i)
    if ((delta_at(i) || held_at(i)) && held.states[i].accepted != request.ballot &&
        (held.states[i].accepted != request.bases[i] || (held_at(i) && !holds_value(held.states[i])) ||
         (!request.summaries.empty() && held.states[i].summary != request.summaries[i])))
      out.answer.need_whole.push_back(i);
  if (!out.answer.need_whole.empty()) return out;
  Held keep = held;
  for (size_t i = 0; i < names; ++i) {
    auto& state = keep.states[i];
    state.promised = request.reserve.value_or(request.ballot);
    state.changed_at = request.changed_at[i];
    state.ancestry = request.ancestry.empty() ? std::vector<Ballot>{} : request.ancestry[i];
    if (held_at(i)) {
      // The value held stays as it is, at the new ballot.
      state.accepted = request.ballot;
      state.value.reset();
      state.has_value = true;
      continue;
    }
    if (delta_at(i)) {
      if (held.states[i].accepted == request.ballot) continue;
      // Applied by the store to what it holds at the base, and summarised.
      state.accepted = request.ballot;
      state.value.reset();
      state.has_value = true;
      state.summary.clear();
      state.delta = request.deltas[i];
      state.base = request.bases[i];
      continue;
    }
    state.accepted = request.ballot;
    state.value = request.values[i];
    state.has_value = request.values[i].has_value();
    // The store summarises the value it takes.
    state.summary.clear();
  }
  if (request.to) {
    // Pending until settled, unless this acceptor already holds the move settled.
    const bool settled = held.slot.served == request.to && !held.slot.pending_from;
    keep.slot.promised = request.ballot;
    keep.slot.served = request.to;
    keep.slot.pending_from = settled ? std::nullopt : std::optional<ReplicaSet>(request.set);
  } else {
    // The proposer had proof the set serves the slot: recorded, and settled.
    keep.slot.served = request.set;
    keep.slot.pending_from.reset();
  }
  out.keep = std::move(keep);
  return out;
}

AcceptStep on_settle(const identity::NodeId& self, const identity::NodeId& from, const Settle& request, const Held& held) {
  AcceptStep out;
  if (!in_either(self, request.from, request.to)) {
    out.answer.refused = refusal("this node is not in the replica set named");
    return out;
  }
  if (!in_either(from, request.from, request.to)) {
    out.answer.refused = refusal("the sender is not in the replica set named");
    return out;
  }
  const auto& slot = held.slot;
  Held keep = held;
  if (slot.served == request.to && slot.pending_from == request.from) {
    keep.slot.pending_from.reset();
  } else if (slot.served == request.from && !slot.pending_from) {
    // Missed the transfer's accept: the move is committed, so it now refuses the old set.
    keep.slot.served = request.to;
  } else if (slot.pending_from == request.from && slot.served != request.to) {
    // A transfer out of the same set to another, which lost: at most one transfer out of a set
    // commits (their majorities of it share an acceptor, which takes only one), and a settle is
    // sent only after a commit. The record pending to the other set is dead.
    keep.slot.served = request.to;
    keep.slot.pending_from.reset();
  } else if (!slot.served && request.to.contains(self)) {
    keep.slot.served = request.to;
  } else {
    return out;  // already settled, or moved on since: nothing to keep
  }
  keep.slot.promised = std::max(keep.slot.promised, request.ballot);
  out.keep = std::move(keep);
  return out;
}

PurgeStep on_purge(const identity::NodeId& self, const identity::NodeId& from, const Purge& request, const Held& held) {
  PurgeStep out;
  out.keep = held;
  if (auto refused = admitted(self, from, request.set, std::nullopt, held, request.names.size(), request.set)) {
    out.refused = std::move(refused);
    return out;
  }
  for (size_t i = 0; i < request.names.size(); ++i) {
    auto& state = out.keep.states[i];
    // Only the tombstone itself: a newer write, or a state that never saw it, stays.
    if (state.accepted != request.ballot || holds_value(state)) continue;
    out.keep.floor = std::max({out.keep.floor, state.promised, state.accepted});
    state = AcceptorState{};
    ++out.forgotten;
  }
  return out;
}

Attempt::Attempt(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, Change change, Reduce reduce, ReplicaSet genesis)
    : _prepare{std::move(set), std::nullopt, slot, ballot, std::move(names), true},
      _change(std::move(change)),
      _reduce(std::move(reduce)),
      _genesis(std::move(genesis)) {
  if (ballot.zero()) throw std::invalid_argument("a ballot's counter starts at one");
  if (!_change) throw std::invalid_argument("an attempt needs its change");
  _recipients = _prepare.set.nodes;
}

Attempt Attempt::transfer(ReplicaSet from, ReplicaSet to, Slot slot, Ballot ballot, Reduce reduce) {
  if (ballot.zero()) throw std::invalid_argument("a ballot's counter starts at one");
  if (!(to.version > from.version)) throw std::invalid_argument("a transfer's new set has a version above the old one's");
  Attempt out;
  out._prepare = Prepare{std::move(from), std::move(to), slot, ballot, {}, true};
  out._reduce = std::move(reduce);
  out._recipients = out._prepare.set.nodes;
  for (const auto& node : out._prepare.to->nodes)
    if (!out._prepare.set.contains(node)) out._recipients.push_back(node);
  std::sort(out._recipients.begin(), out._recipients.end());
  return out;
}

Attempt Attempt::fast(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, Change change, Ballot last,
                      const std::vector<std::optional<Bytes>>& values, const std::vector<Ballot>& changed_at,
                      const std::vector<std::vector<Ballot>>& ancestry) {
  if (!(ballot > last)) throw std::invalid_argument("the fast path's ballot is above its last");
  if (values.size() != names.size() || changed_at.size() != names.size()) throw std::invalid_argument("the fast path's last values name a different count");
  Attempt out(std::move(set), slot, ballot, std::move(names), std::move(change), {}, ReplicaSet{});
  auto changed = out._change(values);
  if (changed.values.size() != values.size()) throw std::logic_error("a change returned a different count of values");
  Accept accept{out._prepare.set, std::nullopt, slot, ballot, out._prepare.names, std::move(changed.values), {}, last, std::nullopt};
  for (size_t i = 0; i < values.size(); ++i) accept.changed_at.push_back(accept.values[i] == values[i] ? changed_at[i] : ballot);
  for (size_t i = 0; i < values.size(); ++i) accept.ancestry.push_back(descend(last, i < ancestry.size() ? ancestry[i] : std::vector<Ballot>{}));
  out._result = std::move(changed.result);
  out._accept = std::move(accept);
  out._phase = Phase::accepting;
  return out;
}

Attempt Attempt::deltas(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, DeltaChange change, ReplicaSet genesis, identity::NodeId self) {
  if (ballot.zero()) throw std::invalid_argument("a ballot's counter starts at one");
  if (!change) throw std::invalid_argument("an attempt needs its change");
  Attempt out;
  out._prepare = Prepare{std::move(set), std::nullopt, slot, ballot, std::move(names), false};
  out._recipients = out._prepare.set.nodes;
  out._genesis = std::move(genesis);
  out._deltas_change = std::move(change);
  out._self = self;
  return out;
}

Attempt Attempt::fast_deltas(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, DeltaChange change, Ballot last,
                             const std::vector<Bytes>& summaries, const std::vector<Ballot>& changed_at, bool local, identity::NodeId self,
                             const std::vector<std::vector<Ballot>>& ancestry) {
  if (!(ballot > last)) throw std::invalid_argument("the fast path's ballot is above its last");
  if (summaries.size() != names.size() || changed_at.size() != names.size()) throw std::invalid_argument("the fast path's last state names a different count");
  auto out = deltas(std::move(set), slot, ballot, std::move(names), std::move(change), ReplicaSet{}, self);
  const size_t count = out._prepare.names.size();
  std::vector<Base> bases;
  for (size_t i = 0; i < count; ++i) bases.push_back(Base{last, summaries[i], true, local});
  auto made = out._deltas_change(bases);
  if (made.decline) {
    out._declined = true;
    out._why = "declined";
    out._phase = Phase::refused;
    return out;
  }
  if ((!made.deltas.empty() && made.deltas.size() != count) || (!made.summaries.empty() && made.summaries.size() != count))
    throw std::logic_error("a value-less change returned a different count");
  made.deltas.resize(count);
  made.summaries.resize(count);
  out._names = out._prepare.names;
  std::vector<Ballot> at(count, last);
  std::vector<uint8_t> has_value(count, 1);
  std::vector<Ballot> moved;
  for (size_t i = 0; i < count; ++i) moved.push_back(made.deltas[i] ? ballot : changed_at[i]);
  auto accept = out.compact(std::move(made.deltas), at, summaries, has_value, moved);
  accept.fast_from = last;
  accept.ancestry.clear();
  for (size_t i = 0; i < count; ++i) accept.ancestry.push_back(descend(last, i < ancestry.size() ? ancestry[i] : std::vector<Ballot>{}));
  for (size_t i = 0; i < count; ++i)
    if (!made.summaries[i]) made.summaries[i] = summaries[i];
  out._new_summaries = std::move(made.summaries);
  out._result = std::move(made.result);
  out._accept = std::move(accept);
  out._phase = Phase::accepting;
  return out;
}

Accept Attempt::compact(std::vector<std::optional<Bytes>> deltas, const std::vector<Ballot>& bases, const std::vector<Bytes>& summaries,
                        const std::vector<uint8_t>& has_value, const std::vector<Ballot>& changed_at) const {
  const size_t count = _prepare.names.size();
  Accept accept{_prepare.set, std::nullopt, _prepare.slot, _prepare.ballot, _prepare.names, std::vector<std::optional<Bytes>>(count),
                changed_at,   std::nullopt, std::nullopt};
  accept.held.assign(count, 0);
  for (size_t i = 0; i < count; ++i)
    if (!deltas[i] && has_value[i]) accept.held[i] = 1;
  accept.deltas = std::move(deltas);
  accept.bases = bases;
  accept.summaries = summaries;
  if (_bases.size() == count) accept.ancestry = descended();
  return accept;
}

std::vector<std::vector<Ballot>> Attempt::descended() const {
  std::vector<std::vector<Ballot>> out;
  for (const auto& base : _bases) out.push_back(descend(base.accepted, base.ancestry));
  return out;
}

std::pair<Attempt::Earlier, const std::pair<Ballot, Bytes>*> Attempt::earlier() const {
  if (_own.empty() || _bases.empty()) return {Earlier::none, nullptr};
  const auto lowest = std::min_element(_own.begin(), _own.end(), [](const auto& a, const auto& b) { return a.first < b.first; })->first;
  const std::pair<Ballot, Bytes>* found = nullptr;
  bool every_in = true;
  bool every_out = true;
  for (const auto& base : _bases) {
    const auto chain = descend(base.accepted, base.ancestry);
    const std::pair<Ballot, Bytes>* hit = nullptr;
    for (const auto& each : _own)
      if (std::find(chain.begin(), chain.end(), each.first) != chain.end()) hit = &each;
    if (hit) {
      every_out = false;
      if (found && found->first != hit->first) every_in = false;
      if (!found) found = hit;
      continue;
    }
    every_in = false;
    // Not in it: known only where the ancestry reaches below every earlier accept.
    if (std::none_of(chain.begin(), chain.end(), [&lowest](const Ballot& each) { return each < lowest; })) every_out = false;
  }
  if (every_in && found) return {Earlier::in, found};
  if (every_out) return {Earlier::none, nullptr};
  return {Earlier::unknown, nullptr};
}

std::optional<Accept> Attempt::delta_bases() {
  const size_t count = _names.size();
  std::vector<Ballot> at;
  std::vector<Bytes> summaries;
  std::vector<uint8_t> has_value;
  std::vector<Ballot> changed_at;
  for (const auto& base : _bases) {
    at.push_back(base.accepted);
    summaries.push_back(base.summary);
    has_value.push_back(base.has_value ? 1 : 0);
    changed_at.push_back(base.changed_at);
  }
  // An earlier accept of this change is in every base (its ancestry says so): its deltas are
  // already in. It is committed as it stands, the ballot only, never applied twice.
  const auto [found, own] = earlier();
  if (found == Earlier::in) {
    _result = own->second;
    _new_summaries.assign(summaries.begin(), summaries.end());
    return compact(std::vector<std::optional<Bytes>>(count), at, summaries, has_value, changed_at);
  }
  std::vector<Base> bases;
  for (size_t i = 0; i < count; ++i) {
    Base base{_bases[i].accepted, _bases[i].summary, _bases[i].has_value, false};
    const auto mine = std::find(_from[i].begin(), _from[i].end(), _self);
    if (mine != _from[i].end()) {
      const auto& answered = _per_name[i][static_cast<size_t>(mine - _from[i].begin())];
      base.local = answered.accepted == base.ballot && answered.summary == base.summary && answered.has_value == base.has_value;
    }
    bases.push_back(std::move(base));
  }
  auto made = _deltas_change(bases);
  if (made.decline) {
    _declined = true;
    _why = "declined";
    _phase = Phase::refused;
    return std::nullopt;
  }
  if ((!made.deltas.empty() && made.deltas.size() != count) || (!made.summaries.empty() && made.summaries.size() != count))
    throw std::logic_error("a value-less change returned a different count");
  made.deltas.resize(count);
  made.summaries.resize(count);
  for (size_t i = 0; i < count; ++i)
    if (!made.summaries[i] && !made.deltas[i]) made.summaries[i] = summaries[i];
  const bool any = std::any_of(made.deltas.begin(), made.deltas.end(), [](const std::optional<Bytes>& delta) { return delta.has_value(); });
  // As change_bases: a delta is never applied where an earlier one of this change may be.
  if (any && !made.repeatable && found == Earlier::unknown) {
    _unknown = true;
    _why = "an earlier accept of this change may be in the value found, so it is not applied again: its outcome is unknown";
    _phase = Phase::refused;
    return std::nullopt;
  }
  _new_summaries = std::move(made.summaries);
  _result = std::move(made.result);
  // A read whose promisers all answered every name with its base: chosen already, by them.
  bool agreed = true;
  for (size_t i = 0; i < count && agreed; ++i)
    for (const auto& answer : _per_name[i])
      if (answer.accepted != _bases[i].accepted || answer.summary != _bases[i].summary || answer.has_value != _bases[i].has_value) agreed = false;
  if (!any && agreed) {
    _accept = Accept{_prepare.set, std::nullopt, _prepare.slot, _prepare.ballot, _names, std::vector<std::optional<Bytes>>(count),
                     changed_at,   std::nullopt, std::nullopt};
    _accepted = _promised;
    _read_only = true;
    _phase = Phase::committed;
    return std::nullopt;
  }
  for (size_t i = 0; i < count; ++i)
    if (made.deltas[i]) changed_at[i] = _prepare.ballot;
  return compact(std::move(made.deltas), at, summaries, has_value, changed_at);
}

// Each recipient is heard once per phase; anything else (a stranger, a repeat) is ignored.
bool Attempt::counted(const identity::NodeId& from) {
  if (!std::binary_search(_recipients.begin(), _recipients.end(), from)) return false;
  if (std::find(_heard.begin(), _heard.end(), from) != _heard.end()) return false;
  _heard.push_back(from);
  return true;
}

bool Attempt::enough(const std::vector<identity::NodeId>& nodes) const {
  const auto count = [&nodes](const ReplicaSet& set) {
    return static_cast<size_t>(std::count_if(nodes.begin(), nodes.end(), [&set](const identity::NodeId& node) { return set.contains(node); }));
  };
  if (count(_prepare.set) < _prepare.set.majority()) return false;
  return !_prepare.to || count(*_prepare.to) >= _prepare.to->majority();
}

void Attempt::refuse(const identity::NodeId& from, const Refusal& refusal) {
  _refusers.push_back(from);
  if (refusal.higher && (!_higher || *refusal.higher > *_higher)) _higher = refusal.higher;
  if (_why.empty()) _why = refusal.why;
  // Served by a configuration as new as this attempt's or newer: this attempt's set is wrong.
  // An older one is a laggard's record, refused like any other.
  if (refusal.serving && !(refusal.serving->version < _prepare.set.version)) {
    // A record of another set held only pending may be a transfer that lost and never
    // committed (the review of 2026-11-17): one acceptor's word, counted as one refusal. Only
    // when every refusal that ends the attempt names that one set is it taken as the set.
    if (refusal.pending_from && *refusal.serving != _prepare.set) {
      if (!_pending_elsewhere || *_pending_elsewhere == *refusal.serving) {
        _pending_elsewhere = refusal.serving;
        ++_pending_count;
      }
    } else {
      _serving = refusal.serving;
      // The set this attempt starts from is the target of an unfinished transfer: the
      // runtime finishes it, then tries again.
      if (refusal.pending_from && *refusal.serving == _prepare.set) _unfinished = refusal.pending_from;
      _phase = Phase::refused;
      return;
    }
  }
  // Too many refusals for a majority of either set.
  const auto refusers = [this](const ReplicaSet& set) {
    return static_cast<size_t>(std::count_if(_refusers.begin(), _refusers.end(), [&set](const identity::NodeId& node) { return set.contains(node); }));
  };
  const auto lost = [&refusers](const ReplicaSet& set) { return refusers(set) > set.nodes.size() - set.majority(); };
  if (lost(_prepare.set) || (_prepare.to && lost(*_prepare.to))) {
    if (_pending_elsewhere && _pending_count == _refusers.size()) _serving = _pending_elsewhere;
    _phase = Phase::refused;
  }
}

std::optional<Accept> Attempt::on_prepare(const identity::NodeId& from, const PrepareAnswer& answer) {
  if (_phase != Phase::preparing || !counted(from)) return std::nullopt;
  if (answer.refused) {
    refuse(from, *answer.refused);
    return std::nullopt;
  }
  const bool shaped = _prepare.to ? answer.states.size() == answer.names.size() : answer.states.size() == _prepare.names.size();
  if (!shaped) {
    refuse(from, Refusal{"a prepare answer named a different count", std::nullopt, std::nullopt, std::nullopt});
    return std::nullopt;
  }
  _promised.push_back(from);
  _answers.push_back(answer);
  if (!enough(_promised)) return std::nullopt;
  auto accept = _prepare.to ? transfer_accept() : plain_accept();
  if (!accept) return std::nullopt;
  return accepting(std::move(*accept));
}

Accept Attempt::accepting(Accept accept) {
  _accept = accept;
  _phase = Phase::accepting;
  _heard.clear();
  _refusers.clear();
  _pending_elsewhere.reset();
  _pending_count = 0;
  _why.clear();
  return accept;
}

bool Attempt::value_less() {
  if (_phase != Phase::preparing || !_promised.empty() || _accept) return false;
  _prepare.values = false;
  return true;
}

std::optional<Accept> Attempt::fetched(size_t id, Bytes value, bool past) {
  if (_phase != Phase::fetching) return std::nullopt;
  const auto wanted = std::find_if(_wanted.begin(), _wanted.end(), [id](const Wanted& each) { return each.id == id; });
  if (wanted == _wanted.end() || (past && !wanted->since)) return std::nullopt;
  if (wanted->answer) {
    auto& answer = _per_name[wanted->name][*wanted->answer];
    answer.value = std::move(value);
    answer.past = past;
  } else {
    _bases[wanted->name].value = std::move(value);
  }
  _wanted.erase(wanted);
  if (!_wanted.empty()) return std::nullopt;
  // Reduced, asking for more (fetching again), refused, or the accept.
  auto accept = reduce_names();
  if (!accept) return std::nullopt;
  return accepting(std::move(*accept));
}

void Attempt::unfetched(std::string why) {
  if (_phase != Phase::fetching) return;
  _wanted.clear();
  _why = std::move(why);
  _phase = Phase::refused;
}

std::optional<Accept> Attempt::plain_accept() {
  const auto& set = _prepare.set;
  const auto settled = std::any_of(_answers.begin(), _answers.end(), [&set](const PrepareAnswer& a) { return a.served == set && !a.pending_from; });
  const auto unrecorded = std::all_of(_answers.begin(), _answers.end(), [](const PrepareAnswer& a) { return !a.served; });
  if (!settled && !(unrecorded && set == _genesis)) {
    const auto pending = std::find_if(_answers.begin(), _answers.end(), [&set](const PrepareAnswer& a) { return a.served == set && a.pending_from; });
    if (pending != _answers.end()) {
      _unfinished = pending->pending_from;
      _serving = set;
      _why = "a transfer of the slot to this set is unfinished";
    } else {
      _why = "no acceptor of this set holds the slot as served by it";
    }
    _phase = Phase::refused;
    return std::nullopt;
  }
  // Each name's answers, in promise order.
  _names = _prepare.names;
  _per_name.clear();
  _from.clear();
  for (size_t i = 0; i < _names.size(); ++i) {
    std::vector<NameState> answers;
    for (const auto& each : _answers) answers.push_back(each.states[i]);
    _per_name.push_back(std::move(answers));
    _from.push_back(_promised);
  }
  return gather();
}

std::optional<Accept> Attempt::gather() {
  // Each name's base: the answer with the highest accepted ballot.
  const size_t count = _names.size();
  _bases.clear();
  for (const auto& answers : _per_name)
    _bases.push_back(*std::max_element(answers.begin(), answers.end(), [](const NameState& a, const NameState& b) { return a.accepted < b.accepted; }));
  // Value-less: a base that holds a value is fetched, by its ballot, from a promiser that
  // answered holding exactly that ballot. One that holds none (never written, or a
  // tombstone) needs nothing.
  _wanted.clear();
  _current.assign(count, std::nullopt);
  _asked.assign(count, {});
  // A value-less change fetches nothing: it never sees a value.
  if (!_prepare.values && !_deltas_change)
    for (size_t i = 0; i < count; ++i)
      if (_bases[i].has_value && !_bases[i].value) want(i, std::nullopt, _bases[i], std::nullopt);
  if (!_wanted.empty()) {
    _phase = Phase::fetching;
    return std::nullopt;
  }
  return reduce_names();
}

void Attempt::want(size_t name, std::optional<size_t> answer, const NameState& of, std::optional<Bytes> since) {
  Wanted wanted{_next_wanted++, name, _names[name], answer, of.accepted, std::move(since), {}};
  // Holders of exactly that state: its ballot, and its summary, since a merge write outside
  // agreement leaves acceptors at one ballot with different values, told apart by summary.
  const auto& answers = _per_name[name];
  for (size_t at = 0; at < answers.size(); ++at) {
    const auto& state = answers[at];
    if (state.has_value && state.accepted == of.accepted && state.summary == of.summary) wanted.holders.push_back(_from[name][at]);
  }
  std::sort(wanted.holders.begin(), wanted.holders.end());
  _wanted.push_back(std::move(wanted));
}

std::optional<Accept> Attempt::reduce_names() {
  const size_t count = _names.size();
  if (_deltas_change) return delta_bases();
  if (!_reduce) {
    std::vector<std::optional<Bytes>> current;
    for (const auto& base : _bases) current.push_back(base.value);
    if (_prepare.to) return transfer_bases(std::move(current));
    return change_bases(std::move(current));
  }
  // Each name not yet reduced; a reduction that needs more asks, and is called again once it
  // is in. Every answer is asked for at most once per name, so this ends.
  for (size_t i = 0; i < count; ++i) {
    while (!_current[i]) {
      auto reduced = _reduce(_bases[i], _per_name[i]);
      if (reduced.need.empty()) {
        _current[i].emplace(std::move(reduced.value));
        break;
      }
      bool fetching = false;
      for (auto& need : reduced.need) {
        auto& answers = _per_name[i];
        auto& asked = _asked[i];
        if (need.answer >= answers.size() || std::find(asked.begin(), asked.end(), need.answer) != asked.end()) {
          _why = "the owner's reduction asked for an answer it was given, or that does not exist";
          _phase = Phase::refused;
          return std::nullopt;
        }
        asked.push_back(need.answer);
        auto& answer = answers[need.answer];
        // Nothing held: given as it is. The base's state, whole: the base's value.
        if (!answer.has_value || answer.value) continue;
        if (!need.since && answer.accepted == _bases[i].accepted && answer.summary == _bases[i].summary) {
          answer.value = _bases[i].value;
          continue;
        }
        want(i, need.answer, answer, std::move(need.since));
        fetching = true;
      }
      if (fetching) break;
    }
  }
  if (!_wanted.empty()) {
    _phase = Phase::fetching;
    return std::nullopt;
  }
  std::vector<std::optional<Bytes>> current;
  for (auto& value : _current) current.push_back(std::move(*value));
  if (_prepare.to) return transfer_bases(std::move(current));
  return change_bases(std::move(current));
}

std::optional<Accept> Attempt::change_bases(std::vector<std::optional<Bytes>> current) {
  const auto& set = _prepare.set;
  const size_t count = _prepare.names.size();
  const auto& bases = _bases;
  // An earlier accept of this change is in every base (its ancestry says so, whoever built on
  // it since): the change is already in the values, which are committed as they are (reduced,
  // with an owner's reduction), with that attempt's result.
  const auto [found, own] = count > 0 ? earlier() : std::pair<Earlier, const std::pair<Ballot, Bytes>*>{Earlier::none, nullptr};
  if (found == Earlier::in) {
    Accept accept{set, std::nullopt, _prepare.slot, _prepare.ballot, _prepare.names, current, {}, std::nullopt, std::nullopt};
    for (const auto& base : bases) accept.changed_at.push_back(base.changed_at);
    accept.ancestry = descended();
    note_unchanged(accept);
    _result = own->second;
    return accept;
  }
  auto changed = _change(current);
  if (changed.values.size() != count) throw std::logic_error("a change returned a different count of values");
  if (!changed.deltas.empty() && changed.deltas.size() != count) throw std::logic_error("a change returned a different count of deltas");
  // Another proposer may have built on an earlier accept of this change: applied again, it
  // could be in the value twice. Only a repeatable change goes on.
  if (!changed.repeatable && found == Earlier::unknown) {
    _unknown = true;
    _why = "an earlier accept of this change may be in the value found, so it is not applied again: its outcome is unknown";
    _phase = Phase::refused;
    return std::nullopt;
  }
  Accept accept{set, std::nullopt, _prepare.slot, _prepare.ballot, _prepare.names, std::move(changed.values), {}, std::nullopt, std::nullopt};
  for (size_t i = 0; i < count; ++i)
    // Unaltered, a value keeps the ballot that last changed it: a stable fencing token.
    accept.changed_at.push_back(accept.values[i] == bases[i].value ? bases[i].changed_at : _prepare.ballot);
  accept.ancestry = descended();
  // Each delta is against the base the change was given.
  if (std::any_of(changed.deltas.begin(), changed.deltas.end(), [](const std::optional<Bytes>& delta) { return delta.has_value(); })) {
    accept.deltas = std::move(changed.deltas);
    for (const auto& base : bases) accept.bases.push_back(base.accepted);
  }
  note_unchanged(accept);
  _result = std::move(changed.result);
  return accept;
}

void Attempt::note_unchanged(const Accept& accept) {
  // A value held and left as it was can go to its holders as the ballot only.
  _unchanged.assign(_names.size(), 0);
  for (size_t i = 0; i < _names.size(); ++i) _unchanged[i] = _bases[i].has_value && _bases[i].value && accept.values[i] == _bases[i].value ? 1 : 0;
}

std::optional<Accept> Attempt::transfer_accept() {
  // Every name any promiser holds, in byte order; a promiser that lacks one answers it as
  // never accepted. Keyed by the bytes as a string (GCC 14 at -O3 misreads a map of byte
  // vectors' compare).
  std::map<std::string, std::vector<NameState>> by_name;
  for (const auto& answer : _answers)
    for (const auto& name : answer.names) by_name.try_emplace(std::string(name.begin(), name.end()));
  for (auto& [key, states] : by_name)
    for (const auto& answer : _answers) {
      NameState state;
      for (size_t i = 0; i < answer.names.size(); ++i)
        if (answer.names[i].size() == key.size() && std::equal(answer.names[i].begin(), answer.names[i].end(), key.begin())) state = answer.states[i];
      states.push_back(std::move(state));
    }
  _names.clear();
  _per_name.clear();
  _from.clear();
  for (auto& [key, answers] : by_name) {
    _names.emplace_back(key.begin(), key.end());
    _per_name.push_back(std::move(answers));
    _from.push_back(_promised);
  }
  return gather();
}

Accept Attempt::transfer_bases(std::vector<std::optional<Bytes>> current) {
  Accept accept{_prepare.set, _prepare.to, _prepare.slot, _prepare.ballot, _names, std::move(current), {}, std::nullopt, std::nullopt};
  for (const auto& base : _bases) accept.changed_at.push_back(base.changed_at);
  accept.ancestry = descended();
  note_unchanged(accept);
  return accept;
}

Accept Attempt::accept_for(const identity::NodeId& node, Accept accept) const {
  // A value-less change has no whole value to send: every recipient gets the compact accept,
  // and one lacking a base asks for it whole.
  if (_deltas_change) return accept;
  const size_t count = accept.names.size();
  if (count != _names.size() || _bases.size() != count) return accept;
  const auto whole = _whole.find(node);
  std::vector<std::optional<Bytes>> deltas(count);
  std::vector<uint8_t> held(count, 0);
  std::vector<Ballot> bases(count);
  std::vector<Bytes> summaries(count);
  bool any = false;
  for (size_t i = 0; i < count; ++i) {
    if (!_bases[i].has_value) continue;
    if (whole != _whole.end() && std::find(whole->second.begin(), whole->second.end(), i) != whole->second.end()) continue;
    // What `node` answered for this name, if it promised: the base's ballot and summary. One
    // not heard from is assumed to hold it; the rule asks whole where it does not.
    const auto& from = _from[i];
    const auto at = std::find(from.begin(), from.end(), node);
    if (at != from.end()) {
      const auto& answered = _per_name[i][static_cast<size_t>(at - from.begin())];
      if (answered.accepted != _bases[i].accepted || answered.summary != _bases[i].summary || !answered.has_value) continue;
    }
    bases[i] = _bases[i].accepted;
    summaries[i] = _bases[i].summary;
    if (i < _unchanged.size() && _unchanged[i]) {
      held[i] = 1;
    } else if (!accept.deltas.empty() && accept.deltas[i]) {
      deltas[i] = accept.deltas[i];
    } else {
      continue;
    }
    accept.values[i].reset();
    any = true;
  }
  if (any) {
    accept.deltas = std::move(deltas);
    accept.held = std::move(held);
    accept.bases = std::move(bases);
    accept.summaries = std::move(summaries);
  } else {
    accept.deltas.clear();
    accept.held.clear();
    accept.bases.clear();
    accept.summaries.clear();
  }
  return accept;
}

std::optional<Accept> Attempt::on_accept(const identity::NodeId& from, const AcceptAnswer& answer) {
  // Committed, it still records who else accepted: the evidence names them all.
  if (_phase == Phase::committed && !answer.refused && answer.need_whole.empty() && counted(from)) {
    _accepted.push_back(from);
    return std::nullopt;
  }
  if (_phase != Phase::accepting || !_accept) return std::nullopt;
  if (!answer.refused && !answer.need_whole.empty()) {
    if (!std::binary_search(_recipients.begin(), _recipients.end(), from) || std::find(_heard.begin(), _heard.end(), from) != _heard.end()) return std::nullopt;
    // A value-less change has nothing whole to send: the acceptor is named, for its owner to
    // catch up, and not counted.
    if (_deltas_change) {
      counted(from);
      _need_whole_nodes.push_back(from);
      refuse(from, refusal("the acceptor needs a name whole, which a value-less change does not carry"));
      return std::nullopt;
    }
    // Asked whole again for a name already sent whole: nothing more to send.
    auto& whole = _whole[from];
    bool more = false;
    for (const auto i : answer.need_whole) {
      if (i >= _accept->names.size() || std::find(whole.begin(), whole.end(), i) != whole.end()) {
        more = false;
        break;
      }
      whole.push_back(i);
      more = true;
    }
    if (more) return accept_for(from, *_accept);
    counted(from);
    refuse(from, refusal("the acceptor asked whole for what it was sent whole"));
    return std::nullopt;
  }
  if (!counted(from)) return std::nullopt;
  if (answer.refused) {
    refuse(from, *answer.refused);
    return std::nullopt;
  }
  _accepted.push_back(from);
  if (enough(_accepted)) _phase = Phase::committed;
  return std::nullopt;
}

}  // namespace dannan::agree
