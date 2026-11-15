//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/agree/caspaxos.h"

#include <algorithm>
#include <stdexcept>

namespace dannan::agree {

ReplicaSet ReplicaSet::of(std::vector<identity::NodeId> nodes, uint64_t version) {
  if (nodes.empty()) throw std::invalid_argument("a replica set has at least one node");
  std::sort(nodes.begin(), nodes.end());
  nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
  return {std::move(nodes), version};
}

bool ReplicaSet::contains(const identity::NodeId& node) const noexcept { return std::binary_search(nodes.begin(), nodes.end(), node); }

namespace {

// Refusals every request meets before its ballot is looked at: who may take part, and
// which set a name is served by.
std::optional<Refusal> admitted(const identity::NodeId& self, const identity::NodeId& from, const ReplicaSet& set, std::span<const AcceptorState> held,
                                size_t names) {
  if (held.size() != names) return Refusal{"the request and the state held name different counts", {}, {}};
  if (!set.contains(self)) return Refusal{"this node is not in the replica set named", {}, {}};
  if (!set.contains(from)) return Refusal{"the sender is not in the replica set named", {}, {}};
  for (const auto& state : held)
    if (state.served && *state.served != set) return Refusal{"a name is served by another replica set", {}, state.served};
  return std::nullopt;
}

}  // namespace

PrepareStep on_prepare(const identity::NodeId& self, const identity::NodeId& from, const Prepare& request, std::span<const AcceptorState> held) {
  PrepareStep out;
  if (auto refused = admitted(self, from, request.set, held, request.names.size())) {
    out.answer.refused = std::move(refused);
    return out;
  }
  // A promise only above every promise and acceptance held, for every name at once.
  Ballot highest;
  for (const auto& state : held) highest = std::max({highest, state.promised, state.accepted});
  if (!(request.ballot > highest)) {
    out.answer.refused = Refusal{"a higher ballot is promised", highest, {}};
    return out;
  }
  std::vector<AcceptorState> keep(held.begin(), held.end());
  for (auto& state : keep) {
    state.promised = request.ballot;
    out.answer.states.push_back({state.accepted, state.value, state.changed_at});
  }
  out.keep = std::move(keep);
  return out;
}

AcceptStep on_accept(const identity::NodeId& self, const identity::NodeId& from, const Accept& request, std::span<const AcceptorState> held) {
  AcceptStep out;
  if (auto refused = admitted(self, from, request.set, held, request.names.size())) {
    out.answer.refused = std::move(refused);
    return out;
  }
  if (request.values.size() != request.names.size() || request.changed_at.size() != request.names.size()) {
    out.answer.refused = Refusal{"the accept names, values and changes differ in count", {}, {}};
    return out;
  }
  // Accepted at the ballot promised or above; equal is the same proposer's retry.
  Ballot highest;
  for (const auto& state : held) highest = std::max({highest, state.promised, state.accepted});
  if (request.ballot < highest) {
    out.answer.refused = Refusal{"a higher ballot is promised", highest, {}};
    return out;
  }
  std::vector<AcceptorState> keep(held.begin(), held.end());
  for (size_t i = 0; i < keep.size(); ++i) {
    keep[i].promised = request.ballot;
    keep[i].accepted = request.ballot;
    keep[i].value = request.values[i];
    keep[i].changed_at = request.changed_at[i];
    keep[i].served = request.set;
  }
  out.keep = std::move(keep);
  return out;
}

Attempt::Attempt(ReplicaSet set, Ballot ballot, std::vector<Name> names, Change change, Reduce reduce)
    : _prepare{std::move(set), ballot, std::move(names)}, _change(std::move(change)), _reduce(std::move(reduce)) {
  if (_prepare.names.empty()) throw std::invalid_argument("an attempt changes at least one name");
  if (ballot.zero()) throw std::invalid_argument("a ballot's counter starts at one");
  if (!_change) throw std::invalid_argument("an attempt needs its change");
}

// Each replica is heard once per phase; anything else (a stranger, a repeat) is ignored.
bool Attempt::counted(const identity::NodeId& from) {
  if (!_prepare.set.contains(from)) return false;
  if (std::find(_heard.begin(), _heard.end(), from) != _heard.end()) return false;
  _heard.push_back(from);
  return true;
}

void Attempt::refuse(const identity::NodeId& from, const Refusal& refusal) {
  _refusers.push_back(from);
  if (refusal.higher && (!_higher || *refusal.higher > *_higher)) _higher = refusal.higher;
  if (refusal.serving) _serving = refusal.serving;
  if (_why.empty()) _why = refusal.why;
  // Another set's name: this attempt's set is wrong, whatever the others say.
  const bool hopeless = refusal.serving || _refusers.size() > _prepare.set.nodes.size() - _prepare.set.majority();
  if (hopeless) _phase = Phase::refused;
}

std::optional<Accept> Attempt::on_prepare(const identity::NodeId& from, const PrepareAnswer& answer) {
  if (_phase != Phase::preparing || !counted(from)) return std::nullopt;
  if (answer.refused) {
    refuse(from, *answer.refused);
    return std::nullopt;
  }
  if (answer.states.size() != _prepare.names.size()) {
    refuse(from, Refusal{"a prepare answer named a different count", {}, {}});
    return std::nullopt;
  }
  _promised.push_back(from);
  _answers.push_back(answer.states);
  if (_promised.size() < _prepare.set.majority()) return std::nullopt;

  // A majority promised: each name's current value from its answers, then the change.
  const size_t count = _prepare.names.size();
  std::vector<NameState> bases;
  std::vector<std::optional<Bytes>> current;
  for (size_t i = 0; i < count; ++i) {
    std::vector<NameState> answers;
    for (const auto& each : _answers) answers.push_back(each[i]);
    const auto base = *std::max_element(answers.begin(), answers.end(), [](const NameState& a, const NameState& b) { return a.accepted < b.accepted; });
    current.push_back(_reduce ? _reduce(base, answers) : base.value);
    bases.push_back(base);
  }
  auto changed = _change(current);
  if (changed.values.size() != count) throw std::logic_error("a change returned a different count of values");
  Accept accept{_prepare.set, _prepare.ballot, _prepare.names, std::move(changed.values), {}};
  for (size_t i = 0; i < count; ++i)
    // Unaltered, a value keeps the ballot that last changed it: a stable fencing token.
    accept.changed_at.push_back(accept.values[i] == bases[i].value ? bases[i].changed_at : _prepare.ballot);
  _result = std::move(changed.result);
  _accept = accept;
  _phase = Phase::accepting;
  _heard.clear();
  _refusers.clear();
  _why.clear();
  return accept;
}

void Attempt::on_accept(const identity::NodeId& from, const AcceptAnswer& answer) {
  if (_phase != Phase::accepting || !counted(from)) return;
  if (answer.refused) return refuse(from, *answer.refused);
  _accepted.push_back(from);
  if (_accepted.size() >= _prepare.set.majority()) _phase = Phase::committed;
}

}  // namespace dannan::agree
