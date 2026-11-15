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
codes and key rotation as registers, the maintainer's ruling of 2026-11-15), Louhi (its scheduler,
to answer). Two phases per change; one when the proposer's ballot is already accepted
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
// strand for the set; may persist before answering: in the stack's
// ack_level names (the maintainer, 2026-11-15, relayed by Kvasir), stores answering from memory
// make a write `decided`, stores syncing first make it `replicated` (Lugh and Louhi in
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
- **TOTP as a register, not a lease: ruled** (the maintainer to Lugh, 2026-11-15, relayed by Lugh):
  Lugh's P8.6 uses this register for TOTP and recovery codes once 0.9.0 ships, and key
  rotation becomes a register change; no interim Dagda lease and no workaround, the maintainer having
  confirmed that his wait-for-agreement ruling supersedes the earlier condition (Q2). A code's time step is accepted at most once
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

## A fencing token (Louhi's answer to question 2)

A strict read is an identity change, which commits the same value under the reader's new
ballot, so the committing ballot changes on every read and cannot fence. So each name also
carries the ballot of the last change that altered its value (`changed_at`): an identity
change, a strict read included, keeps it; a change that alters the value sets it to its
own ballot. `Outcome` reports it per name. It is monotonic per name, never reused, stable
for the life of a value, and comparable by a third party as its 8-byte counter, then the
node id, then the epoch (an encoding is provided). Louhi hands a `run_once` task its
placement's `changed_at` as its fencing token; a task that keeps its own epoch in the value
needs nothing from this. Louhi's uses (per-task placement, address and quota registers)
are its recommendation to the maintainer, who earlier chose placement functions now and a scheduler
lease later; Louhi is putting the conflict with the wait-for-agreement ruling to him.

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

## Reconfiguration, proposed in full (2026-11-15, for review)

Built so far: each acceptor records the set a name is served by and refuses a request
naming another. That closes a change of set only where the two sets' majorities overlap.
Two pieces close the rest.

**1. Moving a name between sets: a joint change, in Dannan.** `transfer(space, names,
from = {S, v}, to = {S', v+1})` runs one change whose prepare and accept must each reach a
majority of S *and* a majority of S'; its accept marks every name served by S' at v+1 at
every acceptor that takes it, in both sets, and carries the current value across (the new
acceptors catch up in the same round). After it commits, a proposer still naming S must
reach a majority of S, which overlaps the moved majority of S, so it meets an acceptor
serving S' and is refused, told the set (the existing `serving` refusal): it re-reads its
placement and retries on S'. Acceptors in S and not in S' keep refusing the name, and may
forget it once the transfer is committed (`forget` scoped to themselves). Grow-then-shrink
one node at a time is the hosted system's choice, not required: the joint change is safe
for any S and S'.

**2. A name no acceptor has seen: its set from an agreed map, the hosted system's.** With
no served set recorded anywhere, two proposers whose rosters differ can name disjoint
majorities, and both commit. No rule at the acceptors can see this (nothing is there to
compare against), so the set a never-touched name starts on must itself be agreed. The
hosted system keeps its placement (Mimir's shard map, Kvasir's range map) as registers in
a space of its own. A proposer takes a name's set from the agreed map, and the map changes
only by agreement, with the transfers it implies. Placement over each node's own roster
(Dannan's `by_capacity`) stays the way to *propose* a new map, never the map itself.

**Open: waits on the maintainer.** the maintainer asked of the root: "Dannan, and its predecessor code in Macha,
were explicitly written to avoid this. What went wrong?" What went wrong: the draft took a
conventional consensus system's shape (a metadata root) where the stack's rule is that no
node is special. The need underneath is real and new: Macha's writes merge, so nodes with
differing rosters are harmless, while agreement on a majority needs every proposer to agree
which set it is a majority of. Dannan's recommended answer has no root at all:

- **A fixed slot table** (Mimir's 256 shards generalised: every name hashes to one of a
  fixed number of slots, fixed for the cluster's life).
- **An untouched slot's set is the genesis members**, a constant fixed when the cluster is
  made and carried with its identity, so every node, however late it joins, computes the
  same set for a slot no acceptor has recorded. No touch, no map, no proposer choice.
- **Each slot's set is recorded in its own acceptors** and moved by `transfer()`, proposed
  by placement as the roster changes, the refusal redirecting a stale proposer. Placement
  moves every slot off the genesis set as members come and go, so after the first moves
  the genesis members are ordinary.
- A hosted system's own map (Kvasir's ranges) lives in slot registers, and its names are
  first touched only after that map commits.
- **Every slot is touched at founding** (Kvasir): one change per slot on the genesis set,
  an empty value, so from the first minute no slot is untouched and the genesis constant is
  used exactly once. Otherwise a slot nobody wrote for years would belong to machines long
  replaced. The rule for an untouched slot stays as the founding moment's and a safety net.
- **Finding a moved slot** (Kvasir): each node advertises, softly through membership, the
  slots its acceptors serve, as **one** advert holding a bitmap (32 bytes for 256 slots;
  no versions, which the refusal supplies). A node holds at most 64 adverts of at most
  1 KiB (`membership/node_record.h:48`), so an advert per slot, or versions per slot
  (2.5 KiB at 256), would not fit. A proposer takes the set from those
  adverts as a hint, never as authority. The acceptors' recorded set, and their refusal
  naming it, stay the truth, so a stale or wrong hint costs one redirect. Without this, a
  late joiner could only ask the genesis members, which would make them special again.

Nothing is a role after genesis; an operator is needed only when a slot's majority is lost
for ever, which no design avoids. The paragraph below is the form reviewed before the maintainer's
question, kept until he rules.

**The root is a genesis value, not a role** (Mimir's condition and Kvasir's concern, both
from the maintainer's rule of no designated nodes). The map registers' first set is the cluster's
founding members, listed in configuration (the list a node joins by). From then on it is an
ordinary register whose set is recorded in itself and changes by `transfer()` like any
other, proposed by the hosted system's placement when a member has been off the roster long
enough. A node whose configuration names an old root finds the current one by the serving
refusal (below). An operator is needed only when a majority of the root is lost for ever,
which no design avoids. While the root has no majority, map changes wait and nothing else
does: commands on names already touched run from a cached map.

**What the hosted systems rely on, stated as contract:**

- A refusal for a set or version the acceptor does not serve carries the set and version it
  does (`Outcome::serving`, built). A proposer with a stale map retries there without
  reading the map. Test: the root unreachable, a moved name is still found and changed.
- **Map first, then touch.** A name is first touched only after a committed map entry names
  its set. A split commits its intent to the map (R becomes R1 and R2 on S), then one change
  on S naming R and the new names, then marks the map done; whoever finds an unfinished
  intent finishes it. That is safe because the intent names one set for the new names, and
  every reader of the map, between the steps or after, takes that set.
- **transfer() of a large value** (Kvasir's ranges, gigabytes mostly in Dagda as shared
  chunks): the new acceptors are caught up beforehand by the store's own means (a manifest
  and the log tail) and say `ready()`. The joint round then carries a version or a delta,
  answered "need whole" otherwise, under a deadline, and the old set serves until it
  commits. This rides on the value-less prepare and deltas, which come first.
- **The fast path across a transfer:** a cached ballot is held for a set and version, so an
  accept naming the old version is refused with the set now served. The last committer
  loses the fast path and never commits once more on the old set. Test named for it.
- **No forced transfer.** A set that has lost its majority for ever cannot be transferred.
  Rebuilding from a surviving replica is the hosted system's recorded operator action under
  a new name or set, not something Dannan offers.

So Dannan provides `transfer()` and states the rule for untouched names; the agreed map is
each hosted system's (Mimir, Kvasir), on the root set they configure. Tests: two proposers
naming different sets for one never-touched name, each from its own roster, can both
commit without the map (the simulator shows it, as the broken protocol the design names);
with sets read from the agreed map, never; and a transfer under every fault keeps the value
and refuses the old set afterwards.

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
