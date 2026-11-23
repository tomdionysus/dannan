//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// Leaderless per-key agreement over the cluster: the runtime around caspaxos.h's rules.

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dannan/agree/caspaxos.h"
#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/net/codec.h"
#include "dannan/net/message.h"

/// Leaderless per-key agreement (TODO/2026-11-15-agreement-design.md, and docs/guide/founding.md):
/// a hosted system serves its space's acceptors with a store of its own, and changes registers from
/// any node; Dannan stores nothing.
namespace dannan::agree {

/// How long an attempt's first round waits for each prepare, accept and fetch answer before a
/// silent acceptor counts as not answering and the attempt retries, or a fetch moves to the next
/// holder; doubled with each attempt of a change, and never past its deadline, so one silent
/// acceptor never holds a change until its deadline.
inline constexpr std::chrono::milliseconds first_round{250};

/// One space's acceptor state, the hosted system's to keep: in memory (MemoryStore), or durably
/// before keep() says it is done.
///
/// Called one request at a time per space; `done` may be called from any thread, never inline from
/// a store's lengthy work on the caller's strand. Names are kept per slot, and each slot has its
/// record.
class AcceptorStore {
 public:
  virtual ~AcceptorStore() = default;
  /// What is held for `names` in `slot`: the slot's record, the space's floor, and one state per name
  /// in order (a name never seen is a default AcceptorState).
  /// @param every Held::names and Held::states go on with every other name held in the slot, in any
  ///   order: a transfer moves them all.
  /// @param values without, each state's value is left empty, with `has_value` and `summary` saying
  ///   what is held (a value-less prepare).
  virtual void load(Slot slot, std::span<const Name> names, bool every, bool values, std::function<void(Held)> done) = 0;
  /// What keep() did: everything, or nothing.
  struct Kept {
    /// Kept, durably.
    bool kept = false;
    /// Not kept, and these names (by index in `held`) asked for whole: their delta could not be
    /// applied. Not kept with this empty: it could not keep at all, and the request is refused.
    std::vector<size_t> need_whole{};
  };
  /// Keeps all of `held` for `slot`, as one durable step, all of it or nothing: the record, the
  /// floor, and each name's state (a default state may be dropped).
  /// - A state carrying a value is given the store's summary of it, at most max_summary bytes.
  /// - A state with `has_value` and no value keeps the value and summary already held.
  /// - A state with a `delta` has it applied, in place, to the value held at exactly its `base`, and
  ///   the result summarised. Every delta is checked before any is applied; one the store cannot
  ///   apply answers `need_whole`, and nothing of `held` is kept.
  ///
  /// "Kept" means it survives a crash at any point after `done`.
  virtual void keep(Slot slot, const Held& held, std::function<void(Kept)> done) = 0;
  /// What value() answers.
  struct Fetched {
    /// The value, or only what lies past the `since` asked with.
    Bytes bytes;
    /// Whether `bytes` is only what lies past `since` (false: the whole value).
    bool past = false;
  };
  /// The value of `name` held at exactly `ballot`, for a proposer's fetch; nothing if the name holds
  /// another ballot or no value. With `since` (a summary this store gave, asked for by the owner's
  /// reduction), it may answer only what it keeps past that summary, in the owner's form, saying so
  /// with `past`; it may always answer whole instead. The default loads the name and answers the
  /// whole value; a store holding large values may answer from where it keeps them.
  virtual void value(Slot slot, const Name& name, const Ballot& ballot, const std::optional<Bytes>& since, std::function<void(std::optional<Fetched>)> done) {
    (void)since;
    load(slot, std::vector<Name>{name}, false, true, [ballot, done = std::move(done)](Held held) {
      if (held.states.size() == 1 && held.states[0].accepted == ballot && held.states[0].value) return done(Fetched{std::move(*held.states[0].value), false});
      done(std::nullopt);
    });
  }
  /// Whether this acceptor may vote: false after a restart that lost its state, until it has caught
  /// up. A store that votes with lost state can commit two values for one name.
  virtual bool ready() const = 0;
  /// Every slot record the store holds, for the slot advert after a start. The default holds none,
  /// and the advert fills as requests arrive.
  virtual void records(std::function<void(std::vector<std::pair<Slot, SlotState>>)> done) { done({}); }
};

/// What an agreement call did, and who said what: never a boolean.
struct Outcome {
  /// Whether it committed.
  bool committed = false;
  /// Committed: one value per name (none: absent).
  std::vector<std::optional<Bytes>> values;
  /// Committed: the ballot that last altered each name (a fencing token).
  std::vector<Ballot> changed_at;
  /// Committed: the change's result, from the attempt that committed.
  Bytes result;
  /// The ballot it committed at.
  Ballot ballot;
  /// The acceptors that took the last round.
  std::vector<identity::NodeId> accepted;
  /// Those that refused it.
  std::vector<identity::NodeId> refused;
  /// Those that did not answer.
  std::vector<identity::NodeId> silent;
  /// Not committed: why. A set refusal names the set the slot is served by.
  std::string why;
  /// Not committed, and an accept of this change may have been taken and built on: it may have taken
  /// effect, once (never twice). Settle it by reading back, or by the owner's own mark in the value.
  /// Never set when committed.
  bool unknown = false;
  /// A set refusal: the set that serves the slot now, and its version.
  std::optional<ReplicaSet> serving;
  /// A transfer: the names it carried to the new set.
  std::vector<Name> names;
  /// Unfinished transfers this call finished on its way.
  uint32_t finished = 0;
  /// Attempts made, retries included.
  uint32_t attempts = 0;
  /// Committed in one round, on the fast path.
  bool fast = false;
  /// forget(): every replica held the tombstone and was told to purge it.
  bool forgotten = false;
  /// change_deltas(): the acceptors that needed a name whole, which it does not carry; the owner
  /// catches them up by its own copy, or by an identity change().
  std::vector<identity::NodeId> need_whole;
};

/// Leaderless per-key agreement over the cluster, as Context::agreement().
///
/// Each space a hosted system serves is divided into slots (slot_of()), each served by a replica set
/// whose acceptors vote on every change to its names. Anything in the stack that can use agreement
/// uses this, and builds no consensus of its own. Every call takes a deadline, an executor and a
/// `done` posted to it once; every call may be made from any thread.
///
/// @code
/// auto& agreement = context.agreement();
/// agreement.serve("mine", "mine", std::make_shared<agree::MemoryStore>(executor, true));  // on every serving node
/// const auto slot = agree::slot_of(name, agreement.table().slots);
/// agreement.change("mine", slot, {name}, replicas, version, [&](auto current) {
///   agree::Changed out;
///   out.values.assign(current.size(), new_value);
///   return out;
/// }, {}, std::chrono::seconds(5), executor, [](agree::Outcome out) {
///   if (!out.committed && out.serving) { /* the slot moved: retry on out.serving */ }
/// });
/// @endcode
class Agreement {
 public:
  /// The way to call a peer: the reply's payload, or nothing and why. Told once.
  using Call = std::function<void(const identity::NodeId& to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline,
                                  std::function<void(std::optional<net::Bytes>, std::string)> done)>;
  /// Whether a node advertises `capability` (a space's owner's).
  using Advertises = std::function<bool(const identity::NodeId& node, const std::string& capability)>;
  /// Runs `task` once `after` has passed on the injected time: retries' backoff and the deadline.
  /// Empty: a timer on the executor reading the time source. The simulation harness gives its own,
  /// so a run's every wait is an event in its world.
  using Schedule = std::function<void(std::chrono::milliseconds after, std::function<void()> task)>;
  /// Sets this node's advert `key` to `value`, or withdraws it (none).
  using Publish = std::function<void(const std::string& key, std::optional<net::Bytes> value)>;
  /// Every node's advert `key`, by node.
  using Adverts = std::function<std::vector<std::pair<identity::NodeId, net::Bytes>>(const std::string& key)>;

  /// The advert a node's served slots of `space` are published under: one bitmap of ceil(slots / 8)
  /// bytes, bit i (slot i % 8 of byte i / 8, low bit first) set where this node's record says it
  /// serves slot i. A hint, never authority: the acceptors' records and their refusals are the truth.
  static std::string slot_advert(const std::string& space) { return "agree.slots/" + space; }
  /// The advert a node's slot table is published under: its fingerprint (table_fingerprint), a u64.
  /// A node whose table differs is refused when it joins, as well as at every agreement request.
  static std::string table_advert() { return "agree.table"; }

  /// The cluster's slot table, fixed for its life.
  struct Table {
    /// The slot count.
    uint32_t slots = default_slots;
    /// The genesis set, which serves a slot no acceptor has recorded, as version 0.
    ReplicaSet genesis;
  };

  /// The node builds its own; a test or the simulation may build one directly.
  /// @param executor runs the proposers' and acceptors' steps.
  /// @param epoch the node's durability epoch, so a restart never reuses a ballot.
  /// @param publish and @p adverts carry the slot advert; without them there is none.
  /// @throws std::invalid_argument for a table of no slots or more than max_slots.
  Agreement(boost::asio::any_io_executor executor, identity::NodeId self, identity::NodeId epoch, Table table, Call call, Advertises advertises,
            const contract::TimeSource* time = &contract::steady_time_source(), Schedule schedule = {}, Publish publish = {}, Adverts adverts = {});
  /// Ends every call in flight.
  ~Agreement();
  /// Not copyable.
  Agreement(const Agreement&) = delete;
  /// Not copyable.
  Agreement& operator=(const Agreement&) = delete;

  /// Serves `space`'s acceptors on this node from `store`, to proposers advertising `capability`
  /// (only the owner's nodes vote or write).
  /// @throws std::invalid_argument for an empty or overlong space, or one already served.
  void serve(std::string space, std::string capability, std::shared_ptr<AcceptorStore> store);
  /// Stops serving `space`.
  void unserve(const std::string& space);

  /// The slot table.
  const Table& table() const noexcept;

  /// The nodes whose advert says they serve `slot` of `space`, sorted: where to look first for a
  /// slot that has moved. A stale or wrong hint costs one refusal, which names the set and its
  /// version. Empty when no node advertises it.
  std::vector<identity::NodeId> hint(const std::string& space, Slot slot) const;

  /// Changes `names`, all of `slot`, together over `replicas` (version `version` of the slot's
  /// configuration): `change` is given their current values and returns the new ones and a result.
  ///
  /// Retries above any higher ballot it hears of, with backoff, finishing a transfer to its set that
  /// it finds unfinished, until committed, refused for its set (`Outcome::serving` says which serves
  /// the slot), or `deadline` passes on the injected time. A change is never applied twice: a retry
  /// that cannot tell whether another proposer built on an earlier accept of it ends
  /// `Outcome::unknown`, unless the change says it is repeatable (Changed::repeatable).
  /// @param reduce how a majority's answers become each name's current value; empty: the
  ///   highest-ballot answer's.
  /// @throws std::invalid_argument for more than one name, until atomic commit (item 26): a change of
  ///   several names could be half taken. A read may name several.
  void change(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Change change, Reduce reduce,
              std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  /// A value-less change: `change` is given each name's base (ballot, summary, whether it holds a
  /// value, whether this node's own acceptor holds exactly it) and returns a delta per name, or
  /// declines. No value is fetched, built or sent: holders take the delta. It commits on a majority
  /// that takes it; an acceptor needing a name whole is not counted and is named in
  /// Outcome::need_whole. `Outcome::values` is empty; `changed_at` is set.
  void change_deltas(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, DeltaChange change,
                     std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  /// A strict read: an identity change, so it returns only what a majority holds; a minority side is
  /// refused. Values in Outcome::values, one per name.
  void read(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Reduce reduce,
            std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  /// Moves `slot` from `from` (version `from_version`) to `to` (a higher version): one round on
  /// majorities of both sets, carrying every name the old set holds, then settled at both, after
  /// which a proposer naming the old set is refused and told the new one. Safe for any two sets;
  /// growing one node at a time is the hosted system's choice. A slot holding more names than one
  /// message carries is refused, saying so.
  void transfer(std::string space, Slot slot, std::vector<identity::NodeId> from, uint64_t from_version, std::vector<identity::NodeId> to, uint64_t to_version,
                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  /// Founds `space`: records every slot as served by the genesis set, one change per slot with no
  /// names, so the genesis constant is used once. Committed when every slot is; otherwise `why` names
  /// the first slot that was not. Repeatable.
  /// @param deadline one deadline for the whole founding (0.12.0): every slot's round ends by it, and
  ///   a slot not begun when it passes is not tried.
  void found(std::string space, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  /// Deletes `names`: commits a tombstone (no value), and only once every replica is seen to hold it,
  /// purges it, each acceptor raising its space's floor past what the names held so no stale
  /// proposer brings a value back. A replica that cannot be reached leaves the tombstone in place,
  /// which reads as absent; `Outcome::why` says so and the call may be repeated. Committed: the
  /// tombstone is committed; forgotten is said in `Outcome::forgotten`.
  void forget(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version,
              std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  /// For a store that restarted without its state (the design's restart rule): a strict read of
  /// `names` at a majority of the other replicas, installed into this node's store for `space` at the
  /// committing ballot, with the slot recorded as served by `replicas`. The store says ready() once
  /// every name it serves has caught up, by this or by its own transfer.
  void catch_up(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version,
                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  /// A peer's agreement request (prepare, accept, purge or settle), answered through `answer`, later.
  /// The node routes them here.
  /// @return the message to answer at once when it cannot be served (an unknown space, a refusal
  ///   before the store is asked), or nothing when the answer will be given.
  std::optional<net::Message> serve_request(const identity::NodeId& from, const net::Message& request, std::function<void(net::Message)> answer);

  /// @cond internal
 private:
  struct Core;
  struct Run;

  // change() without its one-name rule: a read's identity change may name several.
  void any_change(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Change change, Reduce reduce,
                  std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  // Shared with every change in flight, so none outlives what it uses.
  std::shared_ptr<Core> _core;
  /// @endcond
};

}  // namespace dannan::agree
