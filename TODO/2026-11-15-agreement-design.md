# Leaderless per-key agreement in Dannan (0.9.0)

the maintainer, 2026-11-15, Mimir's ruling 6, confirmed to Dannan directly: leaderless consensus per
key is built in Dannan, for Mimir and whoever else needs it (Lugh's per-user TOTP lease,
Louhi's single active scheduler), in 0.9.0. For review by Mimir, Lugh and Dagda before it
is built. Mimir's statement of what it needs is `../mimir/TODO/2026-11-15-mimir-design.md`,
"Agreement in Dannan".

## What it is

CASPaxos (Rystsov, 2018): a linearizable register per name, changed by a function of its
current value, committed by a majority of a replica set the caller names, with no leader
and no log. Users: Mimir (every agreement-class command), Lugh (TOTP replay, recovery
codes, key rotation, all as registers), Louhi (its scheduler, to answer). Two phases per change; one when the proposer's ballot is already accepted
(the fast path below). Dannan stores nothing: each acceptor's state is behind an interface
the hosted system implements, in memory or durably, its choice.

```cpp
namespace dannan::agree {

// A ballot: totally ordered, never reused by any node, across restarts too: a counter,
// then the node's id, then its durability epoch (fresh each start).
struct Ballot { uint64_t counter; identity::NodeId node; identity::NodeId epoch; auto operator<=>(...) = default; };

// One register's accepted state at one acceptor.
struct Accepted { Ballot ballot; std::optional<net::Bytes> value; };

// The hosted system's acceptor state for one replica set, batched so several registers
// of one set move in one round (Mimir's MULTI/EXEC within a slot). Called on Dannan's
// strand for the set; may persist before answering (Mimir's levels; Lugh and Louhi in
// Dagda). Answers within the call's deadline or the round counts the acceptor as silent.
class AcceptorStore {
 public:
  virtual ~AcceptorStore() = default;
  // Phase 1: promise `ballot` for `names` if it is above every promise held for them;
  // the accepted state of each, or a refusal naming the higher promise.
  virtual void prepare(std::span<const std::string> names, Ballot ballot, Done<PrepareAnswer>) = 0;
  // Phase 2: accept `values` under `ballot` unless a higher promise is held.
  virtual void accept(std::span<const std::string> names, Ballot ballot, std::span<const std::optional<net::Bytes>> values, Done<AcceptAnswer>) = 0;
};

// What a change did, and who said what: never a boolean (the stack's rule).
struct Outcome {
  bool committed = false;
  std::vector<std::optional<net::Bytes>> values;   // the committed values, one per name
  Ballot ballot;
  std::vector<identity::NodeId> accepted, refused, silent;
  std::string why;                                  // empty when committed
};

class Agreement {
 public:
  // Changes `names` together on `replicas` (the caller's placement): `change` is given
  // their current values and returns the new ones (or none, a read). Committed when a
  // majority of `replicas` accept. Told once on `executor` within `deadline`; a minority
  // side is refused with why. From any thread.
  void change(std::string space, std::vector<std::string> names, std::vector<identity::NodeId> replicas,
              std::function<std::vector<std::optional<net::Bytes>>(std::span<const std::optional<net::Bytes>>)> change,
              std::chrono::milliseconds deadline, boost::asio::any_io_executor executor, std::function<void(Outcome)> done);
  // A strict read: an identity change, so a value is returned only once a majority has it
  // (Mimir's ruling 5).
  void read(...);
  // The hosted system's acceptor for a space, on this node.
  void serve(std::string space, std::shared_ptr<AcceptorStore> store);
};

}  // namespace dannan::agree
```

A `space` scopes names to one hosted system (`"mimir"`, `"lugh/totp"`), so systems never
collide, and routes the wire messages to that system's `AcceptorStore`.

## The round

1. **Prepare**: the proposer picks a ballot above any it has seen and sends `prepare` to
   every replica. A majority of promises gives it each name's accepted value with the
   highest ballot (CASPaxos's rule: the current value).
2. It applies `change` to the current values.
3. **Accept**: sends the new values under its ballot. A majority of accepts commits.
4. A refusal names the higher ballot: the proposer raises its counter past it and retries
   with backoff, within the deadline. Two proposers racing on one name both progress in
   a few rounds; no leader is needed (Mimir's "two rounds when two nodes race").

**Fast path** (CASPaxos's one-round optimisation): a proposer that committed a name last
caches its ballot and value; its next change on that name sends prepare and accept
together (accept carrying the prepare), committing in one round trip when no one else has
touched it. Mimir's uncontended key costs one round.

## Wire

New messages in Dannan's own range (wire version 2, an addition): `agree_prepare`,
`agree_accept`, and their answers, carried as calls on the control lane at the
foreground class (a client waits on them). One message carries every name of one change.

## Membership and restarts

- The caller supplies `replicas`; Dannan does not choose them. A change in the replica set
  (a node joins, placement moves) is the caller's: CASPaxos's rule for changing the
  acceptor set (grow by one, catch up, then shrink) is documented, and a helper offered for
  the catch-up (read at a majority of the old set, write to the new).
- A node restarting without its acceptor state (Mimir in memory) is a new incarnation:
  its store must refuse to answer until it has caught up, by reading each name at a
  majority of the other replicas. Dannan offers the catch-up read; the store's `ready()`
  says when it may vote. A node that votes with lost state can commit two values for one
  name, so this rule is the store's contract, tested.
- Ballots carry the durability epoch, so a restart never reuses one even with its
  counter lost.

## Mimir's review, taken

- **The committed attempt's result** (Q1). `change` may run several times across retries;
  it returns the new values and an opaque result (the popped element, `SET NX`'s answer, a
  script's return), and `Outcome` carries the result of the attempt that committed, only.
  Scripts run on the proposer, deterministically over the values given; nothing runs at
  an acceptor.
- **One round, kept one** (Q4). No tick or batching delay on the fast path; concurrent
  changes to one replica set are coalesced into one message when they meet, sent at once,
  never held back.
- **Values without shipping them** (ask A). A register may be megabytes (a Redis list).
  Prepare answers with each name's accepted ballot and a small owner summary (a version),
  not its value. The proposer takes the value from its own local acceptor when that holds
  the highest ballot (the usual case: Mimir's proposer is a replica), or fetches it from
  one acceptor that holds it. Accept carries, at the owner's choice, a whole value or a
  delta against a named base ballot, which the `AcceptorStore` applies through the owner's
  function, or answers "need whole" when its accepted ballot is not the base; the proposer
  then sends it whole. Dannan carries the delta as opaque bytes.
- **An owner's reduction, with a stated constraint** (ask B). Mimir's merge-class writes
  change a key's state outside agreement, so two acceptors can hold one highest ballot with
  different states. The reduction of a majority's prepare answers into the current value
  is owner-supplied, defaulting to CASPaxos's highest-ballot rule. **Safety constraint,
  in the contract** (wording Mimir's, 2026-11-15): the reduction is given the
  highest-ballot answer (the base) apart from the rest, and its result is the base with
  zero or more operations applied, each later than the base's commit in the owner's total
  order (Mimir's: the hybrid stamp, then node id) and not already reflected in the base.
  The base is only ever advanced, never rolled back: a later delete or overwrite may remove
  what it holds, an earlier state may never replace it. The reduction is deterministic over
  its inputs, so two proposers given the same answers compute the same result. So a
  committed value is never lost: it is the result, or an ancestor of it under later writes.
  A merge write ordered before the base's commit that has not reached it is the owner's to
  drop and count (Mimir's heal rule). Testable form: every operation applied carries an
  owner order greater than the base's; the owner passes its orders through for the check,
  which the tests make. Dannan cannot prevent a reduction that breaks this; the contract
  states it.
- **The store's own catch-up** (ask C). A restarted store catches up however it chooses
  (Mimir transfers a shard's state from a replica) and says through `ready()` when it may
  vote; the per-name majority read is the default helper, not assumed.
- **Names are bytes** (ask D): any byte, NUL included, on the wire and in the interface.

## Lugh's review, taken

- **Who may take part** (point 1). An acceptor refuses `agree_prepare` and `agree_accept`
  from a node outside the replica set's configuration, and a space is served only to
  nodes advertising its owner's capability (`lugh` for `"lugh/totp"`), declared with
  `serve()`; otherwise any member could vote in, or write, another system's registers.
  Test: a non-member's prepare is refused; a member without the capability is refused.
- **TOTP is a register, not a lease** (Q2). A code's time step is accepted at most once
  per user: `change("lugh/totp", [user], replicas, f)`, with
  `f(last) = step > last ? (step, accepted) : (last, replayed)`; Lugh accepts the code
  only when the committed attempt's result says accepted. No holder, no time, no fence;
  a minority side is refused, so sign-in fails closed. Recovery codes the same way.
- **Singletons as register changes**. Lugh's key rotation is itself a register change
  ("the next key's id and when it signs"), so any node proposes and agreement picks one;
  no lease and no fence. Louhi's scheduler answers for itself; if it needs a fence, the
  committing ballot serves as one.
- A store that persists before answering is within the contract (Lugh will, against a
  majority restarting inside a code's window); `forget()` on user deletion as designed;
  one round uncontended is ample for a sign-in's 15 s step.

## One replica set at a time (Dagda's review, safety 1)

The caller names `replicas` from its own placement, and two proposers whose rosters differ
during a transition can name different sets; a majority of each could then both commit,
two values for one name. So the set is part of the agreed state, not each proposer's view:

- Every prepare and accept carries the replica set's identity (its sorted node ids, hashed)
  and the set's configuration version.
- Each acceptor stores, per space and set, the configuration it serves, and refuses a
  message naming another set or an older version, saying which it serves; the proposer
  then reads the configuration and retries on it.
- A replica set changes only by committing a configuration register through the same
  protocol, on the old set (CASPaxos's grow-catch-up-shrink, one node at a time). The
  hosted system's placement proposes the change; it takes effect when committed.
- Test: two proposers naming different sets for one name never both commit.

## Deleting a name (Dagda's review, safety 2)

Acceptor state otherwise grows without bound (Mimir's `DEL` and expiry), and removing it
naively lets a lagging acceptor bring a value back. CASPaxos's procedure: commit a
tombstone; once every acceptor of the set holds the tombstone, the proposer raises its
ballot counter past it and the acceptors remove the name. Until every acceptor has it, the
tombstone stays. `forget(space, names, replicas, ...)` does this; in 0.9.0's design now, as
Mimir's store contract depends on it, even if the helper comes after the primitive.

## Where a persisting store lives (Dagda's answer to question 3)

On a node-local log, not a head space: each prepare or accept an append under the
acceptor's own dot, group-committed in the node's tick, superseded entries retired, an
in-memory index of the current state per name. A node-local log's no-rollback refuses a
restored older segment at open, which is exactly "restarted without state": the store says
so and refuses to vote until caught up. Two syncs a change (one on the fast path). Until
Dagda's log exists (slice 4, paused for Macha's port), a persisting store is the hosted
system's own file.

## Leases: two guarantees, named (Dagda's review)

Dagda's leases are soft: absence-based, epoch-fenced, two holders possible for the absence
window, available on both sides of a split (Lir's shard leases want this). A register
lease here is linearizable but needs a majority: a minority side cannot take or renew it
(Lugh's TOTP lease and Louhi's single scheduler want this). Each design names which it
uses.

## What it does not do

- No log, no ordering across names beyond one change's names. Cross-slot transactions are
  Mimir's `CROSSSLOT`.
- No leases: a lease is a register whose value says who holds it until when, read at a
  majority (Lugh's and Louhi's use), with time from the hybrid clock.
- Dannan holds no acceptor state, so "stores nothing" holds.

## Tests, first

- One proposer: a change commits on a majority; a read after it sees it.
- Two proposers racing on one name: one value per ballot, both eventually commit in
  order, never two values committed for one round.
- A minority side: refused within the deadline, with `silent` naming the unreachable.
- An acceptor that restarts without state refuses to vote until caught up; one that
  voted anyway is the test that shows the split it causes (documented, not allowed).
- Several names in one change commit together or not at all.
- The fast path: one round trip when uncontended, two when contended.
- Deterministic interleavings with a scripted transport (no sleeps): every order of
  prepare and accept between two proposers and three acceptors keeps the invariant.

## Open for review

1. Mimir: is a function of the current values (run on the proposer) the right form for
   pops, `SET NX` and scripts, or does a script need to run at each acceptor?
2. Lugh, Louhi: is a lease as a register (holder, until, by the hybrid clock) enough for
   the TOTP lease and the single scheduler, or do you need fencing tokens from it? The
   ballot can serve as one.
3. Dagda: should the acceptor stores that persist sit on Dagda's log, and does anything
   here conflict with Dagda's own leases (which are not this guarantee)?
4. Everyone: the fast path's cache is per proposer; is one round for an uncontended key
   enough, or is a Mimir write's latency bound tighter?
