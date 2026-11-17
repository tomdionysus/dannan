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
#include <map>
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

// The owner's summary of one name's value: at most this many bytes, the store's to choose (a
// version, a length, a digest of merge state). Dannan carries it and never reads it.
inline constexpr size_t max_summary = 64;

// A value's ancestry (the review of 2026-11-17): the ballots it was derived through, newest
// first, its base's ballot then the base's own ancestry, at most this many. A zero ballot ends
// it: derived from nothing. A retry reads it to tell whether an earlier accept of its own change
// is in the value it found, so a change is applied once and a contended one still commits.
inline constexpr size_t max_ancestry = 16;
// The ancestry of a value derived from a base accepted at `ballot` whose own ancestry is `of`.
std::vector<Ballot> descend(const Ballot& ballot, const std::vector<Ballot>& of);

// What one acceptor keeps for one name: the hosted system's to hold, in memory or
// durably, and the protocol's to change.
struct AcceptorState {
  Ballot promised;
  Ballot accepted;
  // Empty either when nothing is held or when the state was loaded value-less;
  // `has_value` says which.
  std::optional<Bytes> value;
  // The ballot of the last change that altered the value: a fencing token, stable across
  // identity changes and strict reads (Louhi's review).
  Ballot changed_at;
  // The store's, set by keep() for a value it takes (the design's "Value-less prepare and
  // deltas"). An accept leaves it empty for the store to fill.
  Bytes summary{};
  // Whether a value is held, loaded or not. A state that holds one but carries none is kept
  // with the store's value unchanged.
  bool has_value = false;
  // Only in what an accept gives keep(), never loaded: the delta keep() applies to the value
  // it holds at exactly `base` (the value is empty and `has_value` set). keep() applies every
  // delta and keeps everything, or nothing.
  std::optional<Bytes> delta{};
  Ballot base{};
  // The accepted value's ancestry (max_ancestry). Kept with the state, as the ballots are.
  std::vector<Ballot> ancestry{};
  auto operator<=>(const AcceptorState&) const = default;
};
// Whether a state holds a value, whether it was loaded with it or not.
inline bool holds_value(const AcceptorState& state) noexcept { return state.has_value || state.value.has_value(); }

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

// One name's state as a prepare answer carries it: the value only when the prepare wanted
// values, and always whether one is held and the store's summary of it.
struct NameState {
  Ballot accepted;
  std::optional<Bytes> value;
  Ballot changed_at;
  Bytes summary{};
  bool has_value = false;
  // Fetched for the owner's reduction with a `since`: true when `value` is only what the
  // holder keeps past that summary, false when it is the whole value (a holder may always
  // answer whole; Mimir's digest cannot always be served past).
  bool past = false;
  // The accepted value's ancestry.
  std::vector<Ballot> ancestry{};
};

// A plain change names `set`; a transfer names the set it moves from as `set`, and `to`.
struct Prepare {
  ReplicaSet set;
  std::optional<ReplicaSet> to;
  Slot slot = 0;
  Ballot ballot;
  // A plain change's names; empty for a transfer, which is answered with every name held.
  std::vector<Name> names;
  // Whether the answers carry values. Without, each carries whether it holds one and its
  // summary, and the proposer fetches what it needs.
  bool values = true;
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
  // Deltas (the design's "Value-less prepare and deltas"), one per name or none at all: where
  // `deltas[i]` is set, this acceptor is sent the delta against the value it holds at exactly
  // `bases[i]`, not `values[i]` (left empty); the proposer still knows the whole value.
  std::vector<std::optional<Bytes>> deltas{};
  std::vector<Ballot> bases{};
  // The ballot only (Kvasir's "already held"), one per name or none at all: where `held[i]` is
  // 1, this acceptor already holds the value at exactly `bases[i]`, unchanged by the change,
  // and is sent neither it nor a delta (`values[i]` left empty).
  std::vector<uint8_t> held{};
  // With the compact forms, each one's base summary: an acceptor at the base's ballot whose
  // summary differs (a merge write outside agreement) holds another value, and asks whole.
  // Empty: the ballot alone is checked.
  std::vector<Bytes> summaries{};
  // One per name, or none: each new value's ancestry, kept with it.
  std::vector<std::vector<Ballot>> ancestry{};
};

// Refused; or, for an accept whose deltas this acceptor could not take (its state not at a
// delta's base, or the store's own check), the names it needs whole, by their index in the
// accept, and nothing kept. Otherwise accepted.
struct AcceptAnswer {
  std::optional<Refusal> refused;
  std::vector<size_t> need_whole{};
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
  // Optional, one per name: the owner's delta from the value it was given to `values[i]`, sent
  // instead of the whole value to every acceptor that holds that base. The owner's store
  // applies it in keep(); Dannan never reads it. Empty: every value goes whole.
  std::vector<std::optional<Bytes>> deltas{};
  // Whether this change may be applied to a value that may already hold it: a read, a
  // compare-and-set on a version, a write that recognises its own mark. A retry that cannot
  // tell whether another proposer built on an earlier accept of this change runs a repeatable
  // change again; any other ends not committed, its outcome unknown (the maintainer, 2026-11-17). A
  // repeatable change is always run again on the retry's own base, never handed an earlier
  // one, so an owner's own mark check sees the value its result is applied to (Kvasir's review).
  bool repeatable = false;
};
using Change = std::function<Changed(std::span<const std::optional<Bytes>> current)>;

// What a reduction makes of one name's answers: its value (none: absent); or, before it can
// say, the answers whose values it needs.
struct Reduced {
  // One answer's value, by its index in `answers`: whole, or with `since` (a summary, the
  // owner's) only what the holder keeps past it, in the owner's own form (Mimir: the merge
  // operations past a frontier).
  struct Need {
    size_t answer = 0;
    std::optional<Bytes> since{};
  };
  std::optional<Bytes> value{};
  std::vector<Need> need{};
};

// How a majority's answers for one name become its current value (Mimir's review, ask B,
// and option (a) of the design's "Value-less prepare and deltas"). Given the highest-ballot
// answer (the base), with its value, and every answer, each with its summary; an answer's
// value is there only when the prepare carried values or the reduction asked for it. It
// returns the value, or what it needs; once that is fetched it is called again, with those
// values in. Asking twice for one answer refuses the attempt. The default is the base's
// value. Contract: the result is the base advanced by zero or more operations, each later
// than the base's commit in the owner's total order and not already in it; never rolled
// back to an earlier state; deterministic over its input.
using Reduce = std::function<Reduced(const NameState& base, std::span<const NameState> answers)>;

// The value-less change (part 6, Mimir's and Kvasir's ask): one name's base as it is given
// such a change, with no value. `local`: this node's own acceptor answered holding exactly it
// (on the fast path: this node took the last commit), so the owner may read it in place; the
// owner checks its own state still is that base before relying on it.
struct Base {
  Ballot ballot;
  Bytes summary{};
  bool has_value = false;
  bool local = false;
};
// What a value-less change makes of its bases: a delta per name (none: unchanged) and a
// result; or `decline`, which ends the change at once ("declined"), sending nothing.
// `summaries`, per name: the summary the owner's keep() will set for the base and its delta
// (or the base unchanged). A name without one takes no fast path. A summary that differs from
// what keep() then sets is the owner's bug, costing a slow round, never safety.
struct Deltas {
  std::vector<std::optional<Bytes>> deltas;
  Bytes result;
  bool decline = false;
  std::vector<std::optional<Bytes>> summaries{};
  // As Changed::repeatable.
  bool repeatable = false;
};
using DeltaChange = std::function<Deltas(std::span<const Base> bases)>;

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
  enum class Phase : uint8_t { preparing, fetching, accepting, committed, refused };

  // A value to fetch (the design's "Value-less prepare and deltas"): a name's base, or an
  // answer the owner's reduction asked for. The index of the name, and of the answer (none:
  // the base); the ballot whose value it is; the summary to fetch past (none: the whole
  // value); and the promisers that answered holding exactly that ballot with the same
  // summary, in node order. Any of them holds the same value, so a fetch may move between
  // them. `id` names it to fetched().
  struct Wanted {
    size_t id = 0;
    size_t name = 0;
    // The name itself (a transfer's names are the promisers', not the prepare's).
    Name key;
    std::optional<size_t> answer;
    Ballot ballot;
    std::optional<Bytes> since;
    std::vector<identity::NodeId> holders;
  };

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
  // `ancestry`, one per name or none: the last commit's values' ancestry.
  static Attempt fast(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, Change change, Ballot last,
                      const std::vector<std::optional<Bytes>>& values, const std::vector<Ballot>& changed_at,
                      const std::vector<std::vector<Ballot>>& ancestry = {});
  // A value-less change of `names` by `self` (part 6): value-less, nothing fetched, the change
  // run on the bases. Every recipient is sent the same accept: the deltas, the ballot only for
  // a name left as it is. An acceptor that lacks a base answers need_whole: it is not counted,
  // and is named in need_whole(). Where no name has a delta and every promiser answered every
  // name with its base, that state is chosen already, and the attempt commits with no accept
  // (a current read: one round). Throws std::invalid_argument for a ballot of zero or no change.
  static Attempt deltas(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, DeltaChange change, ReplicaSet genesis, identity::NodeId self);
  // Its fast path: the last commit of `names` by this node was at `last`, reserving `ballot`,
  // leaving each name with `summaries` and `changed_at`; `local` if this node took it. Throws
  // std::invalid_argument for a ballot not above `last` or counts that differ.
  static Attempt fast_deltas(ReplicaSet set, Slot slot, Ballot ballot, std::vector<Name> names, DeltaChange change, Ballot last,
                             const std::vector<Bytes>& summaries, const std::vector<Ballot>& changed_at, bool local, identity::NodeId self,
                             const std::vector<std::vector<Ballot>>& ancestry = {});
  // Whether this attempt is on the fast path.
  bool is_fast() const noexcept { return _accept && _accept->fast_from.has_value(); }
  // A value-less change: whether the owner declined; the acceptors that needed a name whole;
  // each name's summary as the owner said the commit leaves it; whether it committed without an
  // accept.
  bool declined() const noexcept { return _declined; }
  const std::vector<identity::NodeId>& need_whole() const noexcept { return _need_whole_nodes; }
  const std::vector<std::optional<Bytes>>& summaries() const noexcept { return _new_summaries; }
  bool read_only() const noexcept { return _read_only; }
  bool is_deltas() const noexcept { return static_cast<bool>(_deltas_change); }
  // The ballots this proposer's earlier attempts at the same change sent accepts under that an
  // acceptor may have taken, with each one's result. Where a majority's newest value for every
  // name is one of them, the change is already in it: the attempt commits it unchanged and
  // reports that result. Where every name's newest is below all of them, the change cannot be
  // in it, and is applied. Otherwise another proposer may have built on one of them: a
  // repeatable change is applied, and any other refuses the attempt, its outcome unknown. So a
  // change is never applied twice.
  void own(std::vector<std::pair<Ballot, Bytes>> earlier) { _own = std::move(earlier); }
  // Refused because an earlier accept of this change may be in the value found (own()).
  bool unknown() const noexcept { return _unknown; }
  // Asks for no values where it can: a change or a transfer, not yet prepared. Its answers then
  // say what is held, and once enough have promised the attempt is fetching until every base
  // that holds a value, and every answer the reduction asks for, is in. Returns whether it
  // asks for none.
  bool value_less();

  const Prepare& prepare() const noexcept { return _prepare; }
  // The accept, once there is one (at once, on the fast path).
  const std::optional<Accept>& accept() const noexcept { return _accept; }
  // Every node the attempt's messages go to: the set, or both sets of a transfer.
  const std::vector<identity::NodeId>& recipients() const noexcept { return _recipients; }
  // An answer to the prepare; the accept to send to every recipient once enough have
  // promised, and only then.
  std::optional<Accept> on_prepare(const identity::NodeId& from, const PrepareAnswer& answer);
  // Fetching: the values still to fetch.
  const std::vector<Wanted>& wanted() const noexcept { return _wanted; }
  // The value of `wanted()`'s entry `id`, fetched by its ballot: whole, or with `past` only
  // what lies past its `since` (never for an entry without one); the accept to send once
  // everything is in. Ignored unless fetching and still wanted, or `past` with no `since`.
  std::optional<Accept> fetched(size_t id, Bytes value, bool past = false);
  // No holder gave a base's value: refused, for another ballot.
  void unfetched(std::string why);
  // The accept as `node` is sent it, from `accept` (the attempt's, with anything the runtime
  // added). For a name whose base holds a value, unless `node` answered the prepare holding
  // something else or has asked for it whole: the ballot only if the value is unchanged, else
  // the delta if the change gave one, each with the base's ballot and summary. A node not heard
  // from is sent these too, and asks for what it lacks whole. Everything else whole.
  Accept accept_for(const identity::NodeId& node, Accept accept) const;
  // An answer to the accept. An acceptor that needs names whole is not counted: the accept to
  // send it again, with those names whole. One that needs whole a name it was sent whole is
  // counted as refusing.
  std::optional<Accept> on_accept(const identity::NodeId& from, const AcceptAnswer& answer);

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
  // Once every base is in: each name reduced, fetching what a reduction asks for, then the
  // change's accept. None while fetching, or once refused.
  std::optional<Accept> reduce_names();
  // Adds a value to fetch: from the promisers that answered `name` with `of`'s ballot and
  // summary.
  void want(size_t name, std::optional<size_t> answer, const NameState& of, std::optional<Bytes> since);
  // A plain change's accept from its names' current values; none once refused as unknown.
  std::optional<Accept> change_bases(std::vector<std::optional<Bytes>> current);
  // Whether an earlier accept of this change (own()) is in the bases, read from each base's
  // ancestry: in every name through one accept (the change is in, once), in none with every
  // ancestry reaching below them all (it is not), or neither (unknown).
  enum class Earlier : uint8_t { none, in, unknown };
  std::pair<Earlier, const std::pair<Ballot, Bytes>*> earlier() const;
  // Each name's new ancestry, from its base.
  std::vector<std::vector<Ballot>> descended() const;
  // A transfer's accept: every name's current value, unchanged by any change.
  Accept transfer_bases(std::vector<std::optional<Bytes>> current);
  // A value-less change's accept from its bases, or none once committed (read) or declined.
  std::optional<Accept> delta_bases();
  // An accept carrying `deltas` (none: the ballot only, where the base holds a value) against
  // `bases`, with `summaries` the bases' summaries.
  Accept compact(std::vector<std::optional<Bytes>> deltas, const std::vector<Ballot>& bases, const std::vector<Bytes>& summaries,
                 const std::vector<uint8_t>& has_value, const std::vector<Ballot>& changed_at) const;
  // Once `_names`, `_per_name` and `_from` are set: the bases, then fetching or reducing.
  std::optional<Accept> gather();
  void note_unchanged(const Accept& accept);
  std::optional<Accept> transfer_accept();
  // The accept made: the attempt is accepting.
  Accept accepting(Accept accept);

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
  // Once enough have promised: the names (a transfer's, every one any promiser holds), each
  // one's base, its answers and who gave each, and whether the accept leaves it unchanged.
  std::vector<Name> _names;
  std::vector<NameState> _bases;
  std::vector<std::vector<NameState>> _per_name;
  std::vector<std::vector<identity::NodeId>> _from;
  std::vector<uint8_t> _unchanged;
  // Each name's current value once reduced, and the answers its reduction has asked for.
  std::vector<std::optional<std::optional<Bytes>>> _current;
  std::vector<std::vector<size_t>> _asked;
  std::vector<Wanted> _wanted;
  size_t _next_wanted = 0;
  // The names each acceptor asked for whole, by index.
  std::map<identity::NodeId, std::vector<size_t>> _whole;
  // A value-less change's.
  DeltaChange _deltas_change;
  identity::NodeId _self;
  bool _declined = false;
  bool _read_only = false;
  bool _unknown = false;
  // Refusals naming another set held only pending, while they all name one: that set, and how
  // many.
  std::optional<ReplicaSet> _pending_elsewhere;
  size_t _pending_count = 0;
  std::vector<identity::NodeId> _need_whole_nodes;
  std::vector<std::optional<Bytes>> _new_summaries;
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
