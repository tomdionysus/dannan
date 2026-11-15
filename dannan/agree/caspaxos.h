//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dannan/identity/node_id.h"

// Leaderless per-key agreement's rules (TODO/2026-11-15-agreement-design.md): CASPaxos
// (Rystsov, 2018), a linearizable register per name changed by a function of its current
// value and committed by a majority of a replica set, with no leader and no log.
//
// This file is the protocol's pure core: the acceptor's rules over the state the hosted
// system keeps, and one proposer's attempt over the answers it receives. Neither does
// I/O nor reads a clock, so a test drives every interleaving of their messages. Where
// the protocol's safety lives, it is here, written once.
namespace dannan::agree {

using Bytes = std::vector<uint8_t>;
// A register's name: any bytes, NUL included (Mimir's review: Redis keys are binary).
using Name = Bytes;

// Totally ordered, never reused by any node across restarts: a counter, then the node's
// id, then its durability epoch (fresh every process start).
struct Ballot {
  uint64_t counter = 0;
  identity::NodeId node{};
  identity::NodeId epoch{};
  auto operator<=>(const Ballot&) const = default;
  bool zero() const noexcept { return counter == 0; }
};

// The nodes a name's acceptors are, and the configuration's version. Sorted and unique.
struct ReplicaSet {
  std::vector<identity::NodeId> nodes;
  uint64_t version = 0;
  auto operator<=>(const ReplicaSet&) const = default;

  // Sorted and deduplicated; throws std::invalid_argument for an empty set.
  static ReplicaSet of(std::vector<identity::NodeId> nodes, uint64_t version);
  size_t majority() const noexcept { return nodes.size() / 2 + 1; }
  bool contains(const identity::NodeId& node) const noexcept;
};

// What one acceptor keeps for one name: the hosted system's to hold, in memory or
// durably, and the protocol's to change.
struct AcceptorState {
  Ballot promised;
  Ballot accepted;
  std::optional<Bytes> value;
  // The ballot of the last change that altered the value: a fencing token, stable across
  // identity changes and strict reads (Louhi's review).
  Ballot changed_at;
  // The replica set this name is served by, once accepted (Dagda's review, safety 1).
  std::optional<ReplicaSet> served;
};

// One name's state as a prepare answer carries it.
struct NameState {
  Ballot accepted;
  std::optional<Bytes> value;
  Ballot changed_at;
};

struct Prepare {
  ReplicaSet set;
  Ballot ballot;
  std::vector<Name> names;
};

// A refusal names why: a higher ballot to pass, or the replica set this acceptor serves.
struct Refusal {
  std::string why;
  std::optional<Ballot> higher;
  std::optional<ReplicaSet> serving;
};

struct PrepareAnswer {
  std::optional<Refusal> refused;
  // One per name, in the request's order, when promised.
  std::vector<NameState> states;
};

struct Accept {
  ReplicaSet set;
  Ballot ballot;
  std::vector<Name> names;
  std::vector<std::optional<Bytes>> values;
  std::vector<Ballot> changed_at;
};

struct AcceptAnswer {
  std::optional<Refusal> refused;
};

// The acceptor's rules. Given the state held for the request's names (in its order) and
// who sent it, the answer and the state to keep: the hosted system keeps it, durably if
// it is durable, before the answer goes. A request from a node outside the set, to a node
// outside it, or for a name served by another set, is refused (Lugh's and Dagda's
// reviews); a ballot not above every promise is refused naming the highest held.
struct Step {
  // None: refused, and nothing is to be kept.
  std::optional<std::vector<AcceptorState>> keep;
};
struct PrepareStep : Step {
  PrepareAnswer answer;
};
struct AcceptStep : Step {
  AcceptAnswer answer;
};
//
// `floor`: the space's floor, the highest ballot any name this acceptor has forgotten
// held. Every request must be above it, so a forgotten name behaves as though still
// promised at the floor and no stale proposer brings a forgotten value back.
PrepareStep on_prepare(const identity::NodeId& self, const identity::NodeId& from, const Prepare& request, std::span<const AcceptorState> held,
                       const Ballot& floor = {});
AcceptStep on_accept(const identity::NodeId& self, const identity::NodeId& from, const Accept& request, std::span<const AcceptorState> held,
                     const Ballot& floor = {});

// Forgetting names (the design's "Deleting a name", Dagda's review), once a tombstone (no
// value) was committed at `ballot` and every acceptor of the set was seen to hold it. Each
// name is forgotten only where it holds exactly that tombstone, so a newer write, or an
// acceptor that missed the tombstone, keeps its state. `keep` holds a default state for each
// name forgotten (a store may drop such entries) and the rest unchanged; `floor` the floor
// raised past every ballot a forgotten name held.
struct Purge {
  ReplicaSet set;
  Ballot ballot;
  std::vector<Name> names;
};
struct PurgeStep {
  std::optional<Refusal> refused;
  std::vector<AcceptorState> keep;
  Ballot floor;
  size_t forgotten = 0;
};
PurgeStep on_purge(const identity::NodeId& self, const identity::NodeId& from, const Purge& request, std::span<const AcceptorState> held, const Ballot& floor);

// What a change makes of the current values: the new values, one per name (none: absent),
// and an opaque result for the caller (a popped element, SET NX's answer). It may run more
// than once across retries; only the committed attempt's result is the caller's (Mimir's
// review). Deterministic over its input.
struct Changed {
  std::vector<std::optional<Bytes>> values;
  Bytes result;
};
using Change = std::function<Changed(std::span<const std::optional<Bytes>> current)>;

// How a majority's answers for one name become its current value (Mimir's review, ask
// B). Given the highest-ballot answer (the base) and every answer; the default is the
// base's value. Contract: the result is the base advanced by zero or more operations,
// each later than the base's commit in the owner's total order and not already in it;
// never rolled back to an earlier state; deterministic over its input.
using Reduce = std::function<std::optional<Bytes>(const NameState& base, std::span<const NameState> answers)>;

// One proposer's attempt at one change under one ballot: prepare, reduce, change, accept.
// Fed each answer as it arrives, in any order; once committed or refused it ignores more.
class Attempt {
 public:
  enum class Phase : uint8_t { preparing, accepting, committed, refused };

  // Throws std::invalid_argument for no names or a ballot of zero.
  Attempt(ReplicaSet set, Ballot ballot, std::vector<Name> names, Change change, Reduce reduce = {});

  const Prepare& prepare() const noexcept { return _prepare; }
  // An answer to the prepare; the accept to send to every replica once a majority has
  // promised, and only then.
  std::optional<Accept> on_prepare(const identity::NodeId& from, const PrepareAnswer& answer);
  void on_accept(const identity::NodeId& from, const AcceptAnswer& answer);

  Phase phase() const noexcept { return _phase; }
  // Committed: the values and the change's result. Refused: why, and the highest ballot a
  // refusal named, for the next attempt to pass.
  const std::vector<std::optional<Bytes>>& values() const noexcept { return _accept ? _accept->values : _none; }
  const std::vector<Ballot>& changed_at() const noexcept { return _accept ? _accept->changed_at : _no_ballots; }
  const Bytes& result() const noexcept { return _result; }
  const std::string& why() const noexcept { return _why; }
  const std::optional<Ballot>& higher() const noexcept { return _higher; }
  const std::optional<ReplicaSet>& serving() const noexcept { return _serving; }
  // Evidence: who promised or accepted, who refused.
  const std::vector<identity::NodeId>& accepted() const noexcept { return _accepted; }
  const std::vector<identity::NodeId>& refused() const noexcept { return _refusers; }

 private:
  bool counted(const identity::NodeId& from);
  void refuse(const identity::NodeId& from, const Refusal& refusal);

  Prepare _prepare;
  Change _change;
  Reduce _reduce;
  Phase _phase = Phase::preparing;
  std::vector<identity::NodeId> _heard;
  std::vector<std::vector<NameState>> _answers;  // per promise, per name
  std::vector<identity::NodeId> _promised;
  std::vector<identity::NodeId> _accepted;
  std::vector<identity::NodeId> _refusers;
  std::optional<Accept> _accept;
  Bytes _result;
  std::string _why;
  std::optional<Ballot> _higher;
  std::optional<ReplicaSet> _serving;
  std::vector<std::optional<Bytes>> _none;
  std::vector<Ballot> _no_ballots;
};

}  // namespace dannan::agree
