# Leaderless per-key agreement in Dannan (0.9.0)

the maintainer, 2026-11-15, Mimir's ruling 6, confirmed to Dannan directly: leaderless consensus per
key is built in Dannan, for Mimir and whoever else needs it (Lugh's per-user TOTP lease,
Louhi's single active scheduler), in 0.9.0. For review by Mimir, Lugh and Dagda before it
is built. Mimir's statement of what it needs is `../mimir/TODO/2026-11-15-mimir-design.md`,
"Agreement in Dannan".

## What it is

CASPaxos (Rystsov, 2018): a linearizable register per name, changed by a function of its
current value, committed by a majority of a replica set the caller names, with no leader
and no log. Two phases per change; one when the proposer's ballot is already accepted
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
