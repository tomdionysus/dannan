//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

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

// Leaderless per-key agreement over the cluster (TODO/2026-11-15-agreement-design.md):
// the runtime around caspaxos.h's rules. A hosted system serves its space's acceptors with
// a store of its own, and changes registers from any node; Dannan stores nothing.
namespace dannan::agree {

// One space's acceptor state, the hosted system's to keep: in memory, or durably before
// keep() says it is done. Called one request at a time per space; done may be called from
// any thread. Names are kept per slot, and each slot has its record.
class AcceptorStore {
 public:
  virtual ~AcceptorStore() = default;
  // What is held for `names` in `slot`: the slot's record, the space's floor, and one
  // state per name in order (a name never seen is a default AcceptorState). With `every`,
  // `Held::names` and `Held::states` go on with every other name held in the slot, in any
  // order: a transfer moves them all. Without `values`, each state's value is left empty,
  // with `has_value` and `summary` saying what is held (a value-less prepare).
  virtual void load(Slot slot, std::span<const Name> names, bool every, bool values, std::function<void(Held)> done) = 0;
  // What keep() did: everything, or nothing. Not kept with `need_whole` empty: it could not, and
  // the request is refused. Not kept with `need_whole`: the names, by index in `held`, whose
  // delta it could not apply (its own check of the delta against the value), asked for whole.
  struct Kept {
    bool kept = false;
    std::vector<size_t> need_whole{};
  };
  // Keeps all of `held` for `slot`, as one durable step, all of it or nothing (Mimir's and
  // Kvasir's review): the record, the floor, and each name's state (a default state may be
  // dropped).
  // - A state carrying a value is given the store's summary of it, at most max_summary bytes.
  // - A state with `has_value` and no value keeps the value and summary already held.
  // - A state with a `delta` has it applied, in place, to the value held at exactly its
  //   `base` (the rule has checked that the state is there), and the result summarised.
  //   Every delta is checked before any is applied; one the store cannot apply answers
  //   `need_whole`, and nothing of `held` is kept: no ballots and no values. A repeated accept
  //   at a ballot already held never reaches here with its delta: the rule answers it.
  // "Kept" means it survives a crash at any point after done. done may be called from any
  // thread, never inline from a store's lengthy work on the caller's strand.
  virtual void keep(Slot slot, const Held& held, std::function<void(Kept)> done) = 0;
  // What value() answers: the bytes, and whether they are only what lies past the `since` asked
  // with (false: the whole value).
  struct Fetched {
    Bytes bytes;
    bool past = false;
  };
  // The value of `name` held at exactly `ballot`, for a proposer's fetch; nothing if the name
  // holds another ballot or no value. With `since` (a summary this store gave, asked for by the
  // owner's reduction), it may answer only what it keeps past that summary, in the owner's
  // form, saying so with `past`; it may always answer whole instead. `past` without `since` is
  // refused. The default loads the name and answers the whole value; a store holding large
  // values may answer from where it keeps them.
  virtual void value(Slot slot, const Name& name, const Ballot& ballot, const std::optional<Bytes>& since, std::function<void(std::optional<Fetched>)> done) {
    (void)since;
    load(slot, std::vector<Name>{name}, false, true, [ballot, done = std::move(done)](Held held) {
      if (held.states.size() == 1 && held.states[0].accepted == ballot && held.states[0].value) return done(Fetched{std::move(*held.states[0].value), false});
      done(std::nullopt);
    });
  }
  // Whether this acceptor may vote: false after a restart that lost its state, until it
  // has caught up (the design's rule; a store that votes with lost state can commit two
  // values for one name).
  virtual bool ready() const = 0;
  // Every slot record the store holds, for the slot advert after a start. The default holds
  // none, and the advert fills as requests arrive.
  virtual void records(std::function<void(std::vector<std::pair<Slot, SlotState>>)> done) { done({}); }
};

// What a change did, and who said what: never a boolean.
struct Outcome {
  bool committed = false;
  // Committed: one value per name, the ballot that last altered each (a fencing token),
  // and the change's result from the attempt that committed.
  std::vector<std::optional<Bytes>> values;
  std::vector<Ballot> changed_at;
  Bytes result;
  Ballot ballot;
  std::vector<identity::NodeId> accepted, refused, silent;
  // Not committed: why. A set refusal names the set the slot is served by.
  std::string why;
  std::optional<ReplicaSet> serving;
  // A transfer: the names it carried to the new set.
  std::vector<Name> names;
  // Unfinished transfers this call finished on its way.
  uint32_t finished = 0;
  // Attempts made, retries included.
  uint32_t attempts = 0;
  // Committed in one round, on the fast path.
  bool fast = false;
  // forget(): every replica held the tombstone and was told to purge it.
  bool forgotten = false;
};

class Agreement {
 public:
  // The way to call a peer: the reply's payload, or nothing and why. Told once.
  using Call = std::function<void(const identity::NodeId& to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline,
                                  std::function<void(std::optional<net::Bytes>, std::string)> done)>;
  // Whether a node advertises `capability` (a space's owner's).
  using Advertises = std::function<bool(const identity::NodeId& node, const std::string& capability)>;
  // Runs `task` once `after` has passed on the injected time: retries' backoff and the
  // deadline. Empty: a timer on `executor` reading `time`. The simulation harness gives its
  // own, so a run's every wait is an event in its world.
  using Schedule = std::function<void(std::chrono::milliseconds after, std::function<void()> task)>;
  // Sets this node's advert `key` to `value`, or withdraws it (none).
  using Publish = std::function<void(const std::string& key, std::optional<net::Bytes> value)>;
  // Every node's advert `key`, by node.
  using Adverts = std::function<std::vector<std::pair<identity::NodeId, net::Bytes>>(const std::string& key)>;

  // The advert a node's served slots of `space` are published under: one bitmap of
  // ceil(slots / 8) bytes, bit i (slot i % 8 of byte i / 8, low bit first) set where this
  // node's record says it serves slot i. A hint, never authority (the design's "Finding a
  // moved slot"): the acceptors' records and their refusals are the truth.
  static std::string slot_advert(const std::string& space) { return "agree.slots/" + space; }

  // The cluster's slot table, fixed for its life: the count, and the genesis set that
  // serves a slot no acceptor has recorded (the maintainer, 2026-11-15: "No root except at genesis. A
  // root may start with only one node.").
  struct Table {
    uint32_t slots = default_slots;
    ReplicaSet genesis;
  };

  // `executor` runs the proposers' and acceptors' steps; `epoch` is the node's durability
  // epoch, so a restart never reuses a ballot. Throws std::invalid_argument for a table of
  // no slots or more than max_slots.
  // `publish` and `adverts` carry the slot advert; without them there is none.
  Agreement(boost::asio::any_io_executor executor, identity::NodeId self, identity::NodeId epoch, Table table, Call call, Advertises advertises,
            const contract::TimeSource* time = &contract::steady_time_source(), Schedule schedule = {}, Publish publish = {}, Adverts adverts = {});
  ~Agreement();
  Agreement(const Agreement&) = delete;
  Agreement& operator=(const Agreement&) = delete;

  // Serves `space`'s acceptors on this node from `store`, to proposers advertising
  // `capability` (Lugh's review: only the owner's nodes vote or write). Throws
  // std::invalid_argument for an empty or overlong space, or one already served.
  void serve(std::string space, std::string capability, std::shared_ptr<AcceptorStore> store);
  void unserve(const std::string& space);

  const Table& table() const noexcept;

  // The nodes whose advert says they serve `slot` of `space`, sorted: where to look first
  // for a slot that has moved. A stale or wrong hint costs one refusal, which names the set
  // and its version. Empty when no node advertises it.
  std::vector<identity::NodeId> hint(const std::string& space, Slot slot) const;

  // Changes `names`, all of `slot`, together over `replicas` (version `version` of the
  // slot's configuration): `change` is given their current values and returns the new ones
  // and a result. The caller names the slot (slot_of, or the hosted system's own scheme).
  // Retries above any higher ballot it hears of, with backoff, finishing a transfer to its
  // set that it finds unfinished, until committed, refused for its set (`Outcome::serving`
  // says which serves the slot), or `deadline` passes on the injected time. `done` is
  // posted to `executor` once. From any thread.
  void change(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Change change, Reduce reduce,
              std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  // A strict read: an identity change, so it returns only what a majority holds (Mimir's
  // ruling 5); a minority side is refused.
  void read(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version, Reduce reduce,
            std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  // Moves `slot` from `from` (version `from_version`) to `to` (a higher version): one round
  // on majorities of both sets, carrying every name the old set holds, then settled at
  // both, after which a proposer naming the old set is refused and told the new one. Safe
  // for any two sets; growing one node at a time is the hosted system's choice. A slot
  // holding more names than one message carries is refused, saying so.
  void transfer(std::string space, Slot slot, std::vector<identity::NodeId> from, uint64_t from_version, std::vector<identity::NodeId> to, uint64_t to_version,
                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  // Founding (Kvasir's review): records every slot of `space` as served by the genesis set,
  // one change per slot with no names, so the genesis constant is used once. Committed
  // when every slot is; otherwise `why` names the first slot that was not. Repeatable.
  void found(std::string space, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  // Deletes `names` (Dagda's review, safety 2): commits a tombstone (no value), and only
  // once every replica is seen to hold it, purges it, each acceptor raising its space's
  // floor past what the names held so no stale proposer brings a value back. A replica that
  // cannot be reached leaves the tombstone in place, which reads as absent; `Outcome::why`
  // says so and the call may be repeated. Committed: the tombstone is committed; forgotten
  // is said in `Outcome::forgotten`.
  void forget(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version,
              std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  // For a store that restarted without its state (the design's restart rule): a strict read
  // of `names` at a majority of the other replicas, installed into this node's store for
  // `space` at the committing ballot, with the slot recorded as served by `replicas`. The
  // store says ready() once every name it serves has caught up, by this helper or its own
  // transfer (Mimir's review).
  void catch_up(std::string space, Slot slot, std::vector<Name> names, std::vector<identity::NodeId> replicas, uint64_t version,
                std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);

  // A peer's agree_prepare, agree_accept, agree_purge or agree_settle: answered through `answer`, later. Returns the
  // message to answer at once when it cannot be served (an unknown space, a refusal before
  // the store is asked), or nothing when the answer will be given.
  std::optional<net::Message> serve_request(const identity::NodeId& from, const net::Message& request, std::function<void(net::Message)> answer);

 private:
  struct Core;
  struct Run;

  // Shared with every change in flight, so none outlives what it uses.
  std::shared_ptr<Core> _core;
};

}  // namespace dannan::agree
