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

**Ruled (the maintainer, 2026-11-15): "No root except at genesis. A root may start with only one
node."** The form below is the design. The map-register root the draft first proposed is
withdrawn.

**A genesis of one node.** The genesis set may be a single founder, whose majority is
itself: every slot is touched on that one node at founding. As nodes join, placement
grows each slot's set by `transfer()`, a joint change on a majority of the old set and a
majority of the new one (one node, then two of three), so the founder is ordinary once
its slots have moved. Until then the founder is the cluster's single point of failure,
as any one-node cluster is. Tests: found on one node, join four, and every slot ends on a
set of three or more with its value intact, under the simulator's faults.

the maintainer had asked of the first draft's root: "Dannan, and its predecessor code in Macha,
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
  slots its acceptors serve, as **one** advert holding a bitmap of ceil(slots / 8) bytes
  (32 bytes at the default 256; no versions, which the refusal supplies). The slot count
  is configurable and fixed for the cluster's life (the maintainer's ruling for Mimir: "I said
  configurable. 256 default"). It is carried in the cluster's identity beside the genesis
  members, and a node configured with another count is refused at join. The 1 KiB advert
  caps it at 8192 slots. A node holds at most 64 adverts of at most
  1 KiB (`membership/node_record.h:48`), so an advert per slot, or versions per slot
  (2.5 KiB at 256), would not fit. A proposer takes the set from those
  adverts as a hint, never as authority. The acceptors' recorded set, and their refusal
  naming it, stay the truth, so a stale or wrong hint costs one redirect. Without this, a
  late joiner could only ask the genesis members, which would make them special again.

Nothing is a role after genesis; an operator is needed only when a slot's majority is lost
for ever, which no design avoids. (Withdrawn, from the reviewed draft: a map register on
the founding members, moved later by `transfer()`. The slot table needs no map register.)

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

## Reconfiguration: the gap found in building it, and the fix (the maintainer, 2026-11-15)

**The gap.** An acceptor with no record for a name admits a request naming any set
(`agree/caspaxos.cpp`, `admitted`). Suppose a transfer S to S' has its accept reach one node
of S' and a minority of S, and so does not commit. That node now advertises the slot. A
proposer that takes S' from the hint reaches two unrecorded nodes of S' and commits from
nothing. Meanwhile a proposer on S commits on the rest of S. That gives two histories and a
lost value, with no stale proposer at all. A second form: the design records the set per
slot, but the code records it per name, so a name never written inside a moved slot has no
record anywhere and meets the same gap. the maintainer chose the fix below over a review round first.

**The fix, as built:**

1. **The record is per slot.** Every name belongs to one slot, and a change names one slot,
   so its names share one. The caller names the slot (Mimir, 2026-11-15: keys sharing a hash
   tag must land in one shard, so Mimir maps CRC16 of the tag or key onto the slot count).
   `slot_of`, FNV-1a 64 of the name's bytes modulo the count, is the default for a system with
   no scheme of its own. Changes across slots are the atomic commit across replica sets, item 7
   of the plan, which Mimir and Kvasir will propose. The store keeps, beside each slot's names, a slot record that holds:
   - the set the slot is served by;
   - `pending_from`, the set it is moving from, while a transfer is not known to be
     committed;
   - the slot's promise, the highest transfer ballot promised.

   The store loads and keeps the record together with the names, in one call.
2. **A transfer is a change to the whole slot.** Its prepare must be above every promise
   and acceptance held for any name in the slot, and above the slot's promise. It raises
   the slot's promise, and each acceptor answers with every name it holds in the slot. The
   proposer reduces those answers per name and its accept carries them all, so the new
   acceptors catch up in the same round. The accept is refused where any name in the slot
   holds a ballot above it, including names it did not carry. It records S' with
   `pending_from` S. A plain change's ballot must be above the slot's promise, but it does
   not raise that promise, so plain changes to different names in one slot never contend.
3. **Settle.** Once a transfer commits on majorities of both sets, the proposer sends
   `settle` to both sets, which clears `pending_from`. Only a settled record is proof that
   the slot moved.
4. **Admission at an acceptor:**
   - A plain request naming set X:
     - is refused, naming the record, where the slot is recorded for another set, or is
       pending away from X;
     - is promised and answered, saying what the acceptor holds, where the slot is recorded
       for X (settled or pending to X) or not recorded at all.
   - A transfer from S to S' is admitted at a member of S where its record is S settled,
     S' pending from S, or S' settled (finishing it again is harmless), or, if the slot is
     recorded nowhere, only where S is the genesis set. A node only in S' is admitted
     whatever it holds: it is joining, and takes what a majority of S gives it. The old
     set's members vouch for the slot, and a majority of them must promise.
5. **The proposer's rule for a plain change on X**, once a majority of X has promised:
   - It goes on only if a promiser holds X settled, or if X is the genesis set and no
     promiser holds any record. Founding makes the second case one moment's.
   - If a promiser holds X pending from S, the transfer is unfinished: the runtime runs the
     joint change S to X again, settles it, and retries.
   - Otherwise the slot is not served by X (a stale hint), and the change is refused,
     saying so.

   An accept of a plain change on X records X as settled at an acceptor that had no record
   or held it pending, because the proposer has seen the proof.
6. **Founding** is a plain change with no names on the genesis set, once per slot. It
   records the genesis set as settled at each genesis acceptor.

Why it holds:
- A plain commit on X needs a settled record of X in its majority. A record is settled only
  after a transfer committed on majorities of both sets (or at founding).
- A committed transfer's accept reached a majority of S. So a later plain proposer on S
  meets an acceptor in that majority, which refuses it.
- Any S commit ordered before the transfer is either refused by the transfer's accept or
  carried by its prepare, by Paxos's intersection over the slot's ballots.
- An acceptor that missed the transfer only lacks names. Every committed value is still
  held by some acceptor in any majority of S', since the transfer's accept and every later
  commit reached a majority of S'.

Limits, stated:
- A transfer carries whole values. A slot too large for one message is refused, saying so,
  until value-less prepare and deltas let the store catch up beforehand.
- A transfer committed but never settled, whose old set then loses its majority for ever,
  is stuck. That is the "no forced transfer" case, for the hosted system's operator.

Tests:
- the gap's own case (fails on the old rule, passes on this one);
- a plain change on a stale hint is refused;
- an unfinished transfer is finished by the next proposer;
- a name never written in a moved slot is found on the new set;
- a plain change racing a transfer either is carried or refuses the transfer;
- one-node genesis grown to five under the simulator's faults.

## Value-less prepare and deltas (proposed 2026-11-16, for Mimir's and Kvasir's review)

The design's step 2, and the "Values without shipping them" answer to Mimir's ask A, made
concrete. Values can be megabytes (a Redis list), or whole ranges (Kvasir), so neither a
prepare's answers nor an accept should carry them whole when they need not.

**Prepare answers carry no values.** Each name's answer is its accepted ballot, its
`changed_at`, whether it holds a value, and an owner summary: at most 64 bytes, the store's
to choose, such as a version or a length. Dannan never reads it. The value itself stays at
the acceptor.

**The proposer fetches the base.** Once a majority has promised, the base for each name is
the answer with the highest accepted ballot, as now. Its value is taken from:
- this node's own acceptor, when it promised and holds the base ballot (Mimir's usual case:
  the proposer is a replica), read through the store with no message;
- otherwise one promiser that holds the base ballot, by a new `agree_fetch(space, slot,
  name, ballot)`. The answer is the value if that acceptor still holds exactly that ballot,
  or "moved on". On "moved on", the next holder is asked. If no holder answers, the attempt
  retries.

A transfer fetches every name the same way. A name whose base is absent needs no fetch.

**The owner's reduction** sees the base's value and every answer's summary, not every value.
That breaks one case: a reduction that merges other answers' values (Mimir's merge writes)
would also need those values. Two options for Mimir to choose between:
- (a) keep it value-less, and let the reduction ask for the values it needs, by ballot and
  holder, through a fetch callback;
- (b) a space that reduces gets its prepares answered with values, as now.

I recommend (b): it is simple and leaves today's behaviour exactly as it is.

**An accept may carry a delta.** The change returns, for each name, either a whole value or
a delta against the base it was given, which the owner produces. Dannan sends the delta
only to acceptors whose last accepted ballot is that base, which every promiser answered
with. The acceptor applies it through a new store call, `apply(slot, name, base value,
delta)`, which returns the new value. An acceptor that no longer holds the base answers
"need whole" for that name, and the proposer sends it the whole value. Only the delta
crosses the network.

**The store interface after this:**
- `load(slot, names, every, done)` as now, with the states' values left empty when the
  request was value-less;
- `value(slot, name, ballot, done)` for the fetch;
- `apply(...)` for deltas;
- `keep(...)` and `records(...)` as now;
- the summary carried in `AcceptorState`, as the store's to set.

**Wire:** prepare gains a flag for "values wanted". The prepare answer's `NameState` loses
its value and gains the summary and a has-value bit. The accept carries, per name, a whole
value or a delta with its base ballot. The accept answer gains a per-name "need whole" list.
Agreement's wire is unreleased on 0.9, so it changes in place, at version 1.

**Safety:** unchanged. The value a change works from is the base's, which the fetch proves
by its ballot: an acceptor answers only for exactly that ballot. A delta is applied only on
exactly its base. A missing or wrong delta becomes a whole send, never a guess.

**Mimir's review (2026-11-16), taken:**
1. **(a), not (b).** Every Mimir key can take merge writes, so (b) would make every Mimir prepare
   carry whole values again. The reduction is given every answer's summary and the base's value.
   It returns either its result or the names it needs more for; only those are fetched, and
   usually there are none. Mimir's summary is a digest of the key's merge state (its dot
   frontier), so matching summaries mean nothing to fetch.
2. **The fetch:** `value(slot, name, ballot, since_summary)`, where the holder may answer with
   owner-defined bytes past that summary (Mimir: the merge ops past a frontier). The first cut
   may fetch the whole value by ballot, with "since" added later.
3. **`apply` in place:** `apply(slot, name, base_ballot, new_ballot, delta, done(applied |
   need_whole))`. The store applies the delta to what it holds at exactly `base_ballot` and
   keeps the result. Mimir's store holds live lists, hashes and sorted sets, not bytes, and a
   bytes-in, bytes-out apply would rebuild a whole list per write. Dannan never needs the new
   value's bytes, since it only relays the delta: the proposer computes the value itself
   from the base it fetched. So an `AcceptorState` accepted by a delta keeps its ballots in
   `keep()`, and its value stays with the store.
4. `value()` returns bytes (Mimir: Valkey's DUMP form; fetches are rare). `load()` returns
   empty values when value-less. The summary is per name, 64 bytes.

**Kvasir's review (2026-11-16), taken.** The shape fits ranges. A range's value holds its rows and their
intents; a delta is one commit's row changes, and the summary holds the range's version, row count and size. Kvasir
has no merging reduction. Two needs, which replace any store-driven catch-up hook:
1. **Values larger than a message.** `agree_fetch` answers in pieces, resumable by offset under the same ballot:
   each answer is a piece, the total size and the ballot, and a holder that has moved on says so. A "need whole"
   resend goes the same way, piece by piece, accepted only once whole and only at its ballot.
2. **Nothing sent to an acceptor that already holds it.** In `transfer()`, and in any accept, a name that an acceptor
   reported holding at the base ballot is not fetched for it or sent to it: the accept names the ballot only. A
   range's new acceptors are caught up by the store's own means first (a copy from a replica, later Dagda's log);
   the joint round then only checks the ballot.
3. **`apply` in place suits ranges too**, with two points of contract:
   - `done(applied)` means the result is kept as durably as an accept the store took whole would be;
   - a base the store holds but cannot apply the delta to (its own check fails) answers `need_whole`, not an error,
     so the proposer resends whole.

**Found in building part 4 (2026-11-16): `apply()` as its own call breaks atomicity.**
- An accept changes the ballots of every name it carries and, at a delta's name, the value. With a separate
  `apply(slot, name, base_ballot, new_ballot, delta)` followed by `keep()`, a crash between the two leaves the store's
  value at the new ballot while its recorded state says the old one. The next prepare answers the old ballot and the
  new value, so a fetch by ballot gives the wrong bytes.
- With several names, `apply()` can succeed for one name and answer `need_whole` for another. The first is then
  applied though the accept as a whole is not taken.
- **Proposed:** the delta goes into `keep()`. A state in `Held` may carry `delta` and `base` (its base ballot).
  `keep()` applies every delta and keeps the whole of `held`, all of it or nothing, and answers
  `Kept{kept, need_whole}`, naming the names whose delta it could not apply (Kvasir's own-check case). An acceptor
  whose recorded state is not at the base answers `need_whole` from the rule itself, before the store is asked.
  Mimir's and Kvasir's points stand: applied in place, as durable as a whole accept, and no bytes in and out.
- Waiting on the maintainer's choice before part 4 is built.

**Tests named:**
- a megabyte register changed with no prepare answer carrying its value;
- a value larger than one message fetched, and resent whole, in pieces;
- a transfer whose new acceptors already hold the ranges sends no value;
- the fetch from another holder when this node's acceptor is behind;
- a delta refused for a moved base and resent whole;
- a transfer of a slot with large values;
- the simulator with deltas under faults.

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
