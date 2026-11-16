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
    return NameState{state.accepted, request.values ? state.value : std::nullopt, state.changed_at, state.summary, holds_value(state)};
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
  if (request.values.size() != names || request.changed_at.size() != names ||
      (!request.deltas.empty() && (request.deltas.size() != names || request.bases.size() != names))) {
    out.answer.refused = refusal("the accept names, values and changes differ in count");
    return out;
  }
  const auto delta_at = [&request](size_t i) { return !request.deltas.empty() && request.deltas[i].has_value(); };
  for (size_t i = 0; i < names; ++i)
    if (delta_at(i) && request.values[i]) {
      out.answer.refused = refusal("an accept carries a name both whole and as a delta");
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
  for (size_t i = 0; i < names; ++i)
    if (delta_at(i) && held.states[i].accepted != request.ballot && held.states[i].accepted != request.bases[i]) out.answer.need_whole.push_back(i);
  if (!out.answer.need_whole.empty()) return out;
  Held keep = held;
  for (size_t i = 0; i < names; ++i) {
    auto& state = keep.states[i];
    state.promised = request.reserve.value_or(request.ballot);
    state.changed_at = request.changed_at[i];
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
                      const std::vector<std::optional<Bytes>>& values, const std::vector<Ballot>& changed_at) {
  if (!(ballot > last)) throw std::invalid_argument("the fast path's ballot is above its last");
  if (values.size() != names.size() || changed_at.size() != names.size()) throw std::invalid_argument("the fast path's last values name a different count");
  Attempt out(std::move(set), slot, ballot, std::move(names), std::move(change), {}, ReplicaSet{});
  auto changed = out._change(values);
  if (changed.values.size() != values.size()) throw std::logic_error("a change returned a different count of values");
  Accept accept{out._prepare.set, std::nullopt, slot, ballot, out._prepare.names, std::move(changed.values), {}, last, std::nullopt};
  for (size_t i = 0; i < values.size(); ++i) accept.changed_at.push_back(accept.values[i] == values[i] ? changed_at[i] : ballot);
  out._result = std::move(changed.result);
  out._accept = std::move(accept);
  out._phase = Phase::accepting;
  return out;
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
    _serving = refusal.serving;
    // The set this attempt starts from is the target of an unfinished transfer: the
    // runtime finishes it, then tries again.
    if (refusal.pending_from && *refusal.serving == _prepare.set) _unfinished = refusal.pending_from;
    _phase = Phase::refused;
    return;
  }
  // Too many refusals for a majority of either set.
  const auto refusers = [this](const ReplicaSet& set) {
    return static_cast<size_t>(std::count_if(_refusers.begin(), _refusers.end(), [&set](const identity::NodeId& node) { return set.contains(node); }));
  };
  const auto lost = [&refusers](const ReplicaSet& set) { return refusers(set) > set.nodes.size() - set.majority(); };
  if (lost(_prepare.set) || (_prepare.to && lost(*_prepare.to))) _phase = Phase::refused;
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
  _why.clear();
  return accept;
}

bool Attempt::value_less() {
  if (_prepare.to || _phase != Phase::preparing || !_promised.empty() || _accept) return false;
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
  // Each name's base: the answer with the highest accepted ballot.
  const size_t count = _prepare.names.size();
  _bases.clear();
  _per_name.clear();
  for (size_t i = 0; i < count; ++i) {
    std::vector<NameState> answers;
    for (const auto& each : _answers) answers.push_back(each.states[i]);
    _bases.push_back(*std::max_element(answers.begin(), answers.end(), [](const NameState& a, const NameState& b) { return a.accepted < b.accepted; }));
    _per_name.push_back(std::move(answers));
  }
  // Value-less: a base that holds a value is fetched, by its ballot, from a promiser that
  // answered holding exactly that ballot. One that holds none (never written, or a
  // tombstone) needs nothing.
  _wanted.clear();
  _current.assign(count, std::nullopt);
  _asked.assign(count, {});
  if (!_prepare.values)
    for (size_t i = 0; i < count; ++i)
      if (_bases[i].has_value && !_bases[i].value) want(i, std::nullopt, _bases[i], std::nullopt);
  if (!_wanted.empty()) {
    _phase = Phase::fetching;
    return std::nullopt;
  }
  return reduce_names();
}

void Attempt::want(size_t name, std::optional<size_t> answer, const NameState& of, std::optional<Bytes> since) {
  Wanted wanted{_next_wanted++, name, answer, of.accepted, std::move(since), {}};
  // Holders of exactly that state: its ballot, and its summary, since a merge write outside
  // agreement leaves acceptors at one ballot with different values, told apart by summary.
  for (size_t at = 0; at < _answers.size(); ++at) {
    const auto& state = _answers[at].states[name];
    if (state.has_value && state.accepted == of.accepted && state.summary == of.summary) wanted.holders.push_back(_promised[at]);
  }
  std::sort(wanted.holders.begin(), wanted.holders.end());
  _wanted.push_back(std::move(wanted));
}

std::optional<Accept> Attempt::reduce_names() {
  const size_t count = _prepare.names.size();
  if (!_reduce) {
    std::vector<std::optional<Bytes>> current;
    for (const auto& base : _bases) current.push_back(base.value);
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
  return change_bases(std::move(current));
}

Accept Attempt::change_bases(std::vector<std::optional<Bytes>> current) {
  const auto& set = _prepare.set;
  const size_t count = _prepare.names.size();
  const auto& bases = _bases;
  // This proposer's own earlier accept is the newest for every name: its change is already in
  // the values, which are committed as they are, with that attempt's result.
  if (count > 0 && !_reduce) {
    const auto own = std::find_if(_own.begin(), _own.end(), [&bases](const std::pair<Ballot, Bytes>& earlier) {
      return std::all_of(bases.begin(), bases.end(), [&earlier](const NameState& base) { return base.accepted == earlier.first; });
    });
    if (own != _own.end()) {
      Accept accept{set, std::nullopt, _prepare.slot, _prepare.ballot, _prepare.names, current, {}, std::nullopt, std::nullopt};
      for (const auto& base : bases) accept.changed_at.push_back(base.changed_at);
      _result = own->second;
      return accept;
    }
  }
  auto changed = _change(current);
  if (changed.values.size() != count) throw std::logic_error("a change returned a different count of values");
  if (!changed.deltas.empty() && changed.deltas.size() != count) throw std::logic_error("a change returned a different count of deltas");
  Accept accept{set, std::nullopt, _prepare.slot, _prepare.ballot, _prepare.names, std::move(changed.values), {}, std::nullopt, std::nullopt};
  for (size_t i = 0; i < count; ++i)
    // Unaltered, a value keeps the ballot that last changed it: a stable fencing token.
    accept.changed_at.push_back(accept.values[i] == bases[i].value ? bases[i].changed_at : _prepare.ballot);
  // Each delta is against the base the change was given.
  if (std::any_of(changed.deltas.begin(), changed.deltas.end(), [](const std::optional<Bytes>& delta) { return delta.has_value(); })) {
    accept.deltas = std::move(changed.deltas);
    for (const auto& base : bases) accept.bases.push_back(base.accepted);
  }
  _result = std::move(changed.result);
  return accept;
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
  Accept accept{_prepare.set, _prepare.to, _prepare.slot, _prepare.ballot, {}, {}, {}, std::nullopt, std::nullopt};
  for (const auto& [key, answers] : by_name) {
    const auto base = *std::max_element(answers.begin(), answers.end(), [](const NameState& a, const NameState& b) { return a.accepted < b.accepted; });
    accept.names.emplace_back(key.begin(), key.end());
    if (!_reduce) {
      accept.values.push_back(base.value);
    } else {
      // A transfer's answers carry every value, so the reduction has all it can ask for.
      auto reduced = _reduce(base, answers);
      if (!reduced.need.empty()) {
        _why = "the owner's reduction asked for more than a transfer's answers carry";
        _phase = Phase::refused;
        return std::nullopt;
      }
      accept.values.push_back(std::move(reduced.value));
    }
    accept.changed_at.push_back(base.changed_at);
  }
  return accept;
}

Accept Attempt::accept_for(const identity::NodeId& node, Accept accept) const {
  if (accept.deltas.empty()) return accept;
  // What `node` answered the prepare, if it promised.
  const auto promised = std::find(_promised.begin(), _promised.end(), node);
  const PrepareAnswer* answered = promised == _promised.end() ? nullptr : &_answers[static_cast<size_t>(promised - _promised.begin())];
  const auto whole = _whole.find(node);
  bool any = false;
  for (size_t i = 0; i < accept.deltas.size(); ++i) {
    if (!accept.deltas[i]) continue;
    const bool asked_whole = whole != _whole.end() && std::find(whole->second.begin(), whole->second.end(), i) != whole->second.end();
    const bool holds_base = answered && i < answered->states.size() && i < _bases.size() && answered->states[i].accepted == accept.bases[i] &&
                            answered->states[i].summary == _bases[i].summary;
    if (holds_base && !asked_whole) {
      accept.values[i].reset();
      any = true;
    } else {
      accept.deltas[i].reset();
    }
  }
  if (!any) {
    accept.deltas.clear();
    accept.bases.clear();
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
    // Asked whole for a name it was sent whole, or one with no delta: nothing more to send.
    auto& whole = _whole[from];
    bool more = false;
    for (const auto i : answer.need_whole) {
      if (i >= _accept->names.size() || _accept->deltas.empty() || !_accept->deltas[i] || std::find(whole.begin(), whole.end(), i) != whole.end()) {
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
