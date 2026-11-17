# Agreement: a review against the CASPaxos paper (2026-11-17)

Asked by the maintainer after the stall ("watch that commit fail") turned up a second fault: read the
paper, think, then check the rest of the quorum design. The paper is
`research/caspaxos-rystsov-2018-arxiv-1802.07000.pdf` (Rystsov, arXiv 1802.07000 v9), with its
text beside it. Reviewed: `agree/caspaxos.{h,cpp}`, `agree/agreement.{h,cpp}`, and
`2026-11-15-agreement-design.md`.

**Ruled and built (the maintainer, 2026-11-17: "Then do it", and "yes go ahead" for 3, 4 and 6)**, on
`feature/attempt-timeout`; how each was done is under "Resolutions" at the end.

## What the paper says, and does not

- One round: prepare at F+1, take the value with the highest ballot, apply the change, accept
  at F+1, answer the client. The proof (appendix A) is about **acknowledged** changes only:
  any two are in one chain. It needs only that an acceptor never takes a ballot below one it
  saw, and that prepare and accept quorums intersect.
- **It has no retry inside the proposer.** A conflict is returned; "when a proposer receives
  a conflicting message ... it should update its counter". Whether a change that was not
  acknowledged took effect is left open, and its accepted state can still be built on by
  others (the proof's chain runs over accepted states, not acknowledged ones). Every example
  change in the paper is a compare-and-set on a version, which is safe to run again. So my
  "CASPaxos's own answer" was wrong: the paper neither reports "unknown" nor re-applies. The
  re-apply across retries is Dannan's addition (design, "The committed attempt's result"),
  and it is where finding 1 comes from.
- One-round optimisation (2.2.1): the next prepare rides on the accept. Dannan's reservation
  is that, and reads sound (below).
- Membership change (2.3): by flexible quorums, a node at a time, with a re-scan; it warns
  that replacing acceptors without it loses data. Dannan's joint transfer on majorities of
  both sets is stronger and does not need the re-scan.
- Deletion (3.1): tombstone, identity change at all 2F+1, invalidate every proposer's cache,
  fast-forward counters, an age check at acceptors, then remove. Dannan's floor does the work
  of the counter and age steps; its fast-path check covers the cache step. One gap: finding 6.

## Findings

Each of 1 to 4 has a failing test on `feature/attempt-timeout` (node-d, 2026-11-17), and fails
the same on develop's runtime: none comes from the round timeout.

**1. A change can be applied twice and reported committed.** (Safety, for any change that is
not safe to run twice.)
- Mechanism: an attempt's accept reaches a minority. Another proposer's prepare finds it
  newest, builds on it, and is chosen. The first proposer's retry finds a newest value that
  is not its own accept (`Attempt::own`, `caspaxos.cpp:659-672`, matches only the base's
  ballot), so it applies its change again.
- It needs no fault: an accept refused by a majority because of another's prepare is enough.
- Proof: `Agreement.ARetryDoesNotApplyItsChangeAgainOnAValueThatAdoptedItsPartialAccept`
  (the register ends "vaca"). `Simulation.NoChangeIsAppliedTwiceUnderFaults`, each change
  appending its own byte: on **develop's runtime 96 of 600 seeds** hold a byte twice; with the
  round timeout, 146 of 150.
- Why the counter simulations passed: an increment applied twice is explained away by any
  increment that did not commit ("may have taken effect"). The linearizability check on a
  counter cannot see this fault until the extras outnumber the uncommitted changes.
- What it means above: INCR or LPUSH twice; a pop that pops twice and loses an element;
  Lugh's TOTP step answering "replayed" for a first use.
- Recommended rule. Let B be the ballots this change has sent accepts under. On a retry, with
  the base's ballot b:
  - b in B: commit it unchanged, with that attempt's result (as now);
  - b below every ballot in B: the base cannot contain the change (a value never derives from
    a higher ballot's), so apply it (as now);
  - otherwise the base may or may not contain it. End the change **not committed, outcome
    unknown** (`Outcome::unknown`), unless the caller declared the change **repeatable**
    (safe to run on a value that may already hold it: a read, a CAS on a version, a write that
    recognises its own mark). Reads, `catch_up`, `forget`'s tombstone and founding are
    repeatable.
  - Also `unknown` whenever a change ends uncommitted after any accept was sent, the deadline
    included.
- Cost: under contention with faults, Mimir and Kvasir see unknown outcomes for changes they
  do not mark repeatable. Exactly-once is then the owner's, by putting its own operation mark
  in the value (Mimir has dots; the `set_nx` test already does this). Exactly-once inside
  Dannan would need an operation id carried in every stored value: wire and store changes,
  not recommended.

**2. With an owner's reduction the own-accept guard is off** (`caspaxos.cpp:661`,
`count > 0 && !_reduce`), so every retry after a partial accept applies the change again, with
no second proposer. Proof: `Agreement.ARetryWithAReductionAppliesTheChangeOnceToo` ("v++").
Every Mimir change carries a reduction. Fix with 1: the same rule, the reduction still run.

**3. `forget()` never purges, and a repeated accept is refused.** (Function lost.)
- Mechanism: since the fast path, an accept leaves the acceptor promised at the reservation,
  above the accept's own ballot. The same accept again is then "below a promise"
  (`caspaxos.cpp:168-174`). `forget()` re-sends the tombstone's accept to see every replica
  holds it (`agreement.cpp:1026`), so every replica refuses and nothing is purged.
- The design's contract point "a repeated accept is answered as kept" is broken the same
  way, for a duplicated or re-sent accept too; a duplicate's refusal can reach the proposer
  before the first answer and be counted against a change that was accepted.
- Proof: `Simulation.AForgottenNameIsPurgedAtEveryReplica`, three nodes, clean network:
  "the tombstone is committed but not every replica holds it". No test called
  `Agreement::forget` before.
- Recommended: in `on_accept`, an accept whose ballot every named state already holds as
  accepted is answered accepted, nothing kept.

**4. A transfer that lost leaves an acceptor refusing the set that won, for good.**
(Availability; wrong evidence.)
- Mechanism: two transfers leave S for T1 and T2 (two rosters). T1's accept reaches one
  acceptor of S; T2 commits and is settled. That acceptor holds "T1, pending from S" and
  nothing moves it: `on_settle` ignores it, and every T2 request is refused there. Its
  refusal names T1, and one such refusal ends an attempt at once
  (`Attempt::refuse`, `caspaxos.cpp:465-471`), in the prepare or the accept phase, which the
  runtime then reports as final with `serving` = T1, a set that never served the slot. A
  change made at that node hears its own acceptor first, so it always fails.
- Proof: `Reconfiguration.ATransferThatLostDoesNotLeaveAnAcceptorRefusingTheSetThatWon`.
- At most one transfer out of S can commit (their S-majorities share an acceptor, which
  takes one). So, recommended:
  - `on_settle` at an acceptor holding a record pending from `request.from` to another set
    takes `request.to`, settled: a settle is sent only after the commit;
  - a refusal naming a set held only pending is counted as one refusal, not as proof that
    the attempt's set is wrong.

**5. One silent acceptor held a change to its deadline.** Fixed on `feature/attempt-timeout`
by `agree::first_round` (250 ms, doubling per attempt, never past the deadline) and a retry
above the highest ballot a refusal named. It makes finding 1 nearly certain per run, so it
must not merge before 1 and 2.

**6. A wiped acceptor loses the floor (by reading; no test yet).** `catch_up()` installs the
names it read and the slot's record, not the floor. A purged name leaves no state to catch
up, so a wiped acceptor would take a stale accept for it below the floor the others hold,
and a later majority containing it brings the deleted value back (the paper's lost-delete
anomaly). Recommended: `catch_up` raises the floor to its read's ballot, which is above a
majority's floors by construction. Low likelihood (a stale accept surviving a purge, then a
wipe).

## Checked and found sound (by reading)

- **The fast path.** A reservation is a promise given by exactly the acceptors that took the
  last accept; the fast accept is taken only where that promise and that accept still stand;
  the cache entry is taken out by one change at a time (`take_last`), so no ballot is used by
  two attempts; any prepare in between must pass the reservation at a majority.
- **The read with no accept** (`change_deltas`): committed only when every promiser of a
  majority answers the same accepted ballot and summary, which is that value chosen.
- **Transfers**: majorities of both sets in both phases; the proof rule for a plain change;
  the laggard rule; founding and the genesis rule; a lower ballot's value never derives from
  a higher one's.
- **The restart rule**: a majority of the others always meets every quorum the wiped node was
  part of, so its forgotten promises are passed and its forgotten accepts are found, while
  only a minority is wiped at once.
- **Deletion's floor**: covers promises as well as accepts, so a stale accept prepared before
  the tombstone is refused; a fast-path cache from before it is refused by the state check.

## Resolutions (2026-11-17)

- **1 and 2.** `Changed::repeatable` and `Deltas::repeatable` (the change says so, so no
  signature changes); `Outcome::unknown`. A retry's base is compared with the ballots of this
  run's accepts that an acceptor may have taken (`Attempt::own`, `may_hold_earlier`): its own
  accept, committed unchanged; below them all, applied; otherwise a change that is not
  repeatable is refused as unknown. The own check now runs with a reduction too. The runtime
  tallies every accept's answers per ballot, late ones included (`Run::Sent`, `tally`), so an
  accept every recipient refused is no "maybe"; the list is refreshed before each decision. A
  run that ends uncommitted with any accept possibly taken says `unknown`. `read()`,
  `catch_up()`, `forget()`'s tombstone are repeatable.
- A fast-path miss with a silent acceptor is genuinely unknown under this rule: the silent one
  may have taken the fast accept and the contender built on it. `ADeltaIsApplied...` was
  changed to make its "node 2 misses a change" step from the node that committed last; two
  tests' identity changes now say they are repeatable.
- **3.** `on_accept`: every carried name already accepted at the request's ballot, the slot
  recorded as the accept leaves it, is answered as taken with nothing kept (the design's
  contract point 2, which `Deltas.TheRuleAsks...` now states).
- **4.** `on_settle` takes the settled set over a record pending from the same set to another;
  a refusal naming another set held only pending counts as one refusal, and names the set only
  when every refusal that ends the attempt names it.
- **5.** As above; the counter's linearizability budget raised to 20 million (seed 127 needs
  2.05 million once more changes end unknown).
- **6.** `catch_up` raises the floor to its read's ballot. Test:
  `Simulation.ANodeCaughtUpAfterLosingItsStateRefusesAStaleAcceptForAForgottenName`.

Gate with the above: node-d, 856 tests, 855 passed, 1 skipped (as root); every change applied
at most once.

## Measured: the rule costs too much under contention (2026-11-17)

`NoChangeIsAppliedTwiceUnderFaults` counts its outcomes (6000 changes, 40 per seed on one
register from five proposers in two seconds):

| Run | Committed | Unknown: retry could not tell | Unknown: other | Failed, not taken |
|---|---|---|---|---|
| with the simulator's faults | 2161 | 2150 | 800 | 889 |
| contention only, no faults | 2406 | 3141 | 147 | 306 |

Contention alone leaves half the changes unknown, and the design promised two racing
proposers both progress. The ambiguity is real: a partial accept reaches one acceptor before a
contender's prepare, which may or may not see it. The rule cannot tell, so it gives up.

Precise alternative, for the maintainer: **each accepted state carries its recent ancestry**, the ballots
its value was derived through (its base's ballot, then that base's ancestry), bounded (say 16).
A retry then knows: an earlier accept of its own in the base's ancestry means its change is in,
once (commit the value unchanged, with that attempt's result); ancestry complete down below its
first accept without one means it is not (apply); only a truncated ancestry is unknown. That is
an `AcceptorState`, `NameState` and accept wire field, which every hosted store persists.

## Ancestry, prototyped (the maintainer, 2026-11-17: "try it first to see if it solves the problem")

Built on `feature/attempt-timeout`: `AcceptorState`, `NameState` and `Accept` carry each
value's ancestry (`descend`, at most 16 ballots, a zero ballot ending it); the wire carries it
on the prepare answer and the accept; the fast-path cache keeps it. `Attempt::earlier()` reads
it: an earlier accept of this change in every base's ancestry, the change is in, once (commit
as it stands, with that attempt's result); every ancestry below them all without one, it is
not (apply); otherwise unknown unless repeatable.

| Run (6000 changes) | Committed | Unknown: retry could not tell | Unknown: other | Failed |
|---|---|---|---|---|
| faults, rule only | 2161 | 2150 | 800 | 889 |
| faults, with ancestry | 3098 | 417 | 1513 | 972 |
| contention only, rule only | 2406 | 3141 | 147 | 306 |
| contention only, with ancestry | 2753 | 347 | 1639 | 1261 |

- It solves the problem it was for: the retries that could not tell fall by 80 to 89 per cent,
  and no change is applied twice anywhere.
- What is left unknown is mostly a change that kept retrying under five-way contention on one
  register and ran out of its 3-second deadline after an accept had gone out. That is
  throughput under contention, a separate question; develop's runtime has the same runs end
  uncommitted, unflagged (and some of them applied twice).
- The 347 to 417 still unknown come from ancestries cut short: a fast-path chain or a long run
  of reads past 16, or a state caught up by `catch_up` (installed with none).
- Cost found: the counter's linearizability check needs up to 94 million search steps (a
  minute on a Pi), as more changes commit late; budget raised to 200 million. A
  counter-specific check would remove it.
- Gate: node-d, 858 tests, 856 passed, 1 skipped (as root), and finding 7's test failing by
  design.

## Finding 7: a change of several names can be half taken (proven)

`Agreement.AChangeOfSeveralNamesIsNeverHalfTaken`: node 1 changes x and y together, accepted by
acceptor 1 only. Node 2 changes x alone; its majority shows node 1's accept newest for x, so x
carries it on. Node 3 changes y alone on acceptors that never saw it. x then holds node 1's
change and y does not. Mimir's MULTI/EXEC within a slot, and any change of several names,
relies on all or nothing. Per-name ancestry makes it detectable, not prevented: a proposer
adopting part of a multi-name accept must adopt all of it, which means a change of a subset of
names has to see, and carry, the other names of any multi-name accept it builds on. Not yet
designed.
