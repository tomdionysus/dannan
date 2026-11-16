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

// The nodes a slot's acceptors are, and the configuration's version. Sorted and unique.
struct ReplicaSet {
  std::vector<identity::NodeId> nodes;
  uint64_t version = 0;
  auto operator<=>(const ReplicaSet&) const = default;

  // Sorted and deduplicated; throws std::invalid_argument for an empty set.
  static ReplicaSet of(std::vector<identity::NodeId> nodes, uint64_t version);
  size_t majority() const noexcept { return nodes.size() / 2 + 1; }
  bool contains(const identity::NodeId& node) const noexcept;
};

// Every name belongs to one slot of a fixed table, and a slot's record says which set
// serves it (the design's "Reconfiguration"). The caller names the slot: a hosted system
// with its own scheme (Mimir's hash tags) computes it; slot_of is the default, FNV-1a 64
// over the name's bytes modulo `count`, fixed for the cluster's life.
using Slot = uint32_t;
inline constexpr uint32_t default_slots = 256;
inline constexpr uint32_t max_slots = 8192;
// Throws std::invalid_argument for a count of zero or above max_slots.
Slot slot_of(std::span<const uint8_t> name, uint32_t count);

// What one acceptor keeps for one name: the hosted system's to hold, in memory or
// durably, and the protocol's to change.
struct AcceptorState {
  Ballot promised;
  Ballot accepted;
  std::optional<Bytes> value;
  // The ballot of the last change that altered the value: a fencing token, stable across
  // identity changes and strict reads (Louhi's review).
  Ballot changed_at;
  auto operator<=>(const AcceptorState&) const = default;
};

// What one acceptor keeps for one slot. Unrecorded (no `served`) until a change or a
// transfer is accepted for it.
struct SlotState {
  // The set the slot is served by.
  std::optional<ReplicaSet> served;
  // The set it is moving from while a transfer is not known to be committed; cleared by
  // settle, or by a plain change accepted on `served`.
  std::optional<ReplicaSet> pending_from;
  // The highest transfer ballot promised: a plain change must be above it.
  Ballot promised;
  auto operator<=>(const SlotState&) const = default;
};

// What one acceptor holds for one request: the slot's record, the space's floor (the
// highest ballot any name this acceptor has forgotten held), and the names' states, one per
// name. For a transfer, `names` is the request's names and then every other name held in
// the slot.
struct Held {
  SlotState slot;
  Ballot floor;
  std::vector<Name> names;
  std::vector<AcceptorState> states;
};

// One name's state as a prepare answer carries it.
struct NameState {
  Ballot accepted;
  std::optional<Bytes> value;
  Ballot changed_at;
};

// A plain change names `set`; a transfer names the set it moves from as `set`, and `to`.
struct Prepare {
  ReplicaSet set;
  std::optional<ReplicaSet> to;
  Slot slot = 0;
  Ballot ballot;
  // A plain change's names; empty for a transfer, which is answered with every name held.
  std::vector<Name> names;
};

// A refusal names why: a higher ballot to pass, or the set this acceptor serves the slot by
// (and, while a transfer is unfinished, the set it is moving from).
struct Refusal {
  std::string why;
  std::optional<Ballot> higher;
  std::optional<ReplicaSet> serving;
  std::optional<ReplicaSet> pending_from;
};

struct PrepareAnswer {
  std::optional<Refusal> refused;
  // The slot's record at this acceptor: the proposer's proof of which set serves it.
  std::optional<ReplicaSet> served;
  std::optional<ReplicaSet> pending_from;
  // A transfer's: every name held in the slot. A plain change's: empty.
  std::vector<Name> names;
  // One per name, in the request's order (a transfer's: in `names`' order), when promised.
  std::vector<NameState> states;
};

struct Accept {
  ReplicaSet set;
  std::optional<ReplicaSet> to;
  Slot slot = 0;
  Ballot ballot;
  std::vector<Name> names;
  std::vector<std::optional<Bytes>> values;
  std::vector<Ballot> changed_at;
  // The fast path (the design's "Fast path", CASPaxos's one-round optimisation). A plain
  // accept may `reserve` the proposer's next ballot: an acceptor that takes it promises the
  // reservation, not the accept's own ballot. The proposer's next change of the same names is
  // then an accept at the reserved ballot with `fast_from` its last ballot, taken only where
  // every name still holds that reservation as its promise and `fast_from` as accepted, the
  // slot served by the set with no transfer promised since. Any prepare that wins in between
  // must pass the reservation at a majority, so it outranks the fast value wherever both are.
  std::optional<Ballot> fast_from{};
  std::optional<Ballot> reserve{};
};

struct AcceptAnswer {
  std::optional<Refusal> refused;
};

// After a transfer committed on majorities of both sets: the slot's move is settled.
struct Settle {
  ReplicaSet from;
  ReplicaSet to;
  Slot slot = 0;
  Ballot ballot;
};

// The acceptor's rules. Given what is held for the request and who sent it, the answer and
// what to keep: the hosted system keeps it, durably if it is durable, before the answer
// goes. Refused, and nothing kept: a request from a node outside the set, or to one; a
// plain request for a slot served by another set or moving away from it; a transfer
// from a set the slot is not served by (or, for a slot recorded nowhere, from any set but
// `genesis`); a ballot not above every promise held, naming the highest (Lugh's and
// Dagda's reviews, and the design's "Reconfiguration: the gap ... and the fix").
//
// Every request must be above the floor, so a forgotten name behaves as though still
// promised at the floor and no stale proposer brings a forgotten value back.
struct Step {
  // None: refused, and nothing is to be kept.
  std::optional<Held> keep;
};
struct PrepareStep : Step {
  PrepareAnswer answer;
};
struct AcceptStep : Step {
  AcceptAnswer answer;
};
PrepareStep on_prepare(const identity::NodeId& self, const identity::NodeId& from, const Prepare& request, const Held& held, const ReplicaSet& genesis);
AcceptStep on_accept(const identity::NodeId& self, const identity::NodeId& from, const Accept& request, const Held& held, const ReplicaSet& genesis);
// Clears a pending move to `to` from `from`. A record already elsewhere is left alone.
AcceptStep on_settle(const identity::NodeId& self, const identity::NodeId& from, const Settle& request, const Held& held);

// Forgetting names (the design's "Deleting a name", Dagda's review), once a tombstone (no
// value) was committed at `ballot` and every acceptor of the set was seen to hold it. Each
// name is forgotten only where it holds exactly that tombstone, so a newer write, or an
// acceptor that missed the tombstone, keeps its state. `keep` holds a default state for each
// name forgotten (a store may drop such entries) and the rest unchanged, and the floor
// raised past every ballot a forgotten name held.
struct Purge {
  ReplicaSet set;
  Slot slot = 0;
  Ballot ballot;
  std::vector<Name> names;
};
struct PurgeStep {
  std::optional<Refusal> refused;
  Held keep;
  size_t forgotten = 0;
};
PurgeStep on_purge(const identity::NodeId& self, const identity::NodeId& from, const Purge& request, const Held& held);

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

// One proposer's attempt under one ballot: prepare, reduce, change, accept. Fed each answer
// as it arrives, in any order; once committed or refused it ignores more.
//
// A plain change goes on to its accept only with proof that its set serves the slot: a
// promiser holding the set settled, or, for a slot no promiser has recorded, the set being
// `genesis`. A promiser holding the set pending from another means a transfer is
// unfinished: the attempt is refused naming it (`unfinished()`), for the runtime to finish.
//
// A transfer moves a whole slot: prepare and accept must each reach a majority of both
// sets, and its accept carries every name the promisers hold, reduced, unchanged.
class Attempt {
 public:
  enum class Phase : uint8_t { preparing, accepting, committed, refused };

  // A plain change of `names` (none: the slot's record alone, as founding writes it).
  // Throws std::invalid_argument for a ballot of zero or no change.
  Attempt(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, Change change, Reduce reduce, ReplicaSet genesis);
  // A transfer of `slot` from `from` to `to`. Throws std::invalid_argument for a ballot of
  // zero, or a `to` whose version is not above `from`'s.
  static Attempt transfer(ReplicaSet from, ReplicaSet to, Slot slot, Ballot ballot, Reduce reduce = {});
  // The fast path: a plain change by the proposer whose change of `names` at `last`, which
  // reserved `ballot`, was the last committed, with `values` and `changed_at` as it committed
  // them. One round: the attempt starts accepting at the reserved ballot. Throws
  // std::invalid_argument for a ballot not above `last`.
  static Attempt fast(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, Change change, Ballot last,
                      const std::vector<std::optional<Bytes>>& values, const std::vector<Ballot>& changed_at);
  // Whether this attempt is on the fast path.
  bool is_fast() const noexcept { return _accept && _accept->fast_from.has_value(); }
  // The ballots this proposer's earlier attempts at the same change sent accepts under, with
  // each one's result. Where a majority's newest value for every name is one of them, the
  // change is already in it: the attempt commits it unchanged and reports that result, so a
  // retry after a partial accept never applies the change twice.
  void own(std::vector<std::pair<Ballot, Bytes>> earlier) { _own = std::move(earlier); }

  const Prepare& prepare() const noexcept { return _prepare; }
  // The accept, once there is one (at once, on the fast path).
  const std::optional<Accept>& accept() const noexcept { return _accept; }
  // Every node the attempt's messages go to: the set, or both sets of a transfer.
  const std::vector<identity::NodeId>& recipients() const noexcept { return _recipients; }
  // An answer to the prepare; the accept to send to every recipient once enough have
  // promised, and only then.
  std::optional<Accept> on_prepare(const identity::NodeId& from, const PrepareAnswer& answer);
  void on_accept(const identity::NodeId& from, const AcceptAnswer& answer);

  Phase phase() const noexcept { return _phase; }
  // Committed: the names (a transfer's: every name it carried), the values and the change's
  // result. Refused: why, and the highest ballot a refusal named, for the next attempt.
  const std::vector<Name>& names() const noexcept { return _accept ? _accept->names : _prepare.names; }
  const std::vector<std::optional<Bytes>>& values() const noexcept { return _accept ? _accept->values : _none; }
  const std::vector<Ballot>& changed_at() const noexcept { return _accept ? _accept->changed_at : _no_ballots; }
  const Bytes& result() const noexcept { return _result; }
  const std::string& why() const noexcept { return _why; }
  const std::optional<Ballot>& higher() const noexcept { return _higher; }
  const std::optional<ReplicaSet>& serving() const noexcept { return _serving; }
  // Refused because a transfer to this set from the set named is unfinished.
  const std::optional<ReplicaSet>& unfinished() const noexcept { return _unfinished; }
  // Evidence: who promised or accepted, who refused.
  const std::vector<identity::NodeId>& accepted() const noexcept { return _accepted; }
  const std::vector<identity::NodeId>& refused() const noexcept { return _refusers; }

 private:
  Attempt() = default;
  bool counted(const identity::NodeId& from);
  void refuse(const identity::NodeId& from, const Refusal& refusal);
  // Whether `nodes` hold a majority of the set, and of `to` for a transfer.
  bool enough(const std::vector<identity::NodeId>& nodes) const;
  std::optional<Accept> plain_accept();
  std::optional<Accept> transfer_accept();

  Prepare _prepare;
  std::vector<identity::NodeId> _recipients;
  Change _change;
  Reduce _reduce;
  ReplicaSet _genesis;
  std::vector<std::pair<Ballot, Bytes>> _own;
  Phase _phase = Phase::preparing;
  std::vector<identity::NodeId> _heard;
  std::vector<PrepareAnswer> _answers;  // per promise
  std::vector<identity::NodeId> _promised;
  std::vector<identity::NodeId> _accepted;
  std::vector<identity::NodeId> _refusers;
  std::optional<Accept> _accept;
  Bytes _result;
  std::string _why;
  std::optional<Ballot> _higher;
  std::optional<ReplicaSet> _serving;
  std::optional<ReplicaSet> _unfinished;
  std::vector<std::optional<Bytes>> _none;
  std::vector<Ballot> _no_ballots;
};

}  // namespace dannan::agree
