# Dannan - Active Work

Last updated: 2026-11-15 (rationalised: history to `COMPLETED.md`). Dannan is written in this repository as its own
library, Macha's code at `00861dd` being the source of truth, and Macha is ported
onto it.
That reverses the morning's "in-tree first" (the maintainer, 2026-11-03: "I
misplanned. It should be here first."). "Resume here" below says what is
built. `COMPLETED.md` is the record; move items across as they land, with what
shipped and the gate counts.

The design is the maintainer's, 2026-11-02, in two documents: "Dannan: Distributed
Substrate Library Plan" (the library in full) and "Macha Core Extraction
Plan" (the seams, now read as port steps). Both are rendered here as
[`2026-11-02-dannan-plan.md`](2026-11-02-dannan-plan.md) and
[`2026-11-02-macha-core-extraction-plan.md`](2026-11-02-macha-core-extraction-plan.md);
these copies are the masters (the maintainer, 2026-11-04: the Claude Docs the
projects started from are starters only, to be deleted, never read for
direction or edited). Where the two differ the Dannan plan is the later
and wins. The Dannan plan's "Repository, build, versioning" now says
Dannan is its own repository from the start (the maintainer's decision above). The port of Macha is
[`2026-11-03-macha-extraction.md`](2026-11-03-macha-extraction.md): its
file table and each seam's "what must not change" and gate still hold,
its direction is reversed, and a note at its head says how to read it.
The list of everything Macha does that Dannan and Dagda must do, and that
Macha must still do once ported, is
[`2026-11-03-macha-function-inventory.md`](2026-11-03-macha-function-inventory.md).

**The sources, as of 2026-11-04.** `../macha` is at 0.92.2 (`5776392`),
cluster protocol 23, state layout v18, 785 test cases (744 + 20 + 21). The
live cluster is live-a and live-b, both Pi 5 on Debian 13 with Boost
1.83. `../athenasip` is on `develop` at `8d65593`, tagged `0.12.0`; plugin
contract `API_VERSION` 24. The laptop is on MacPorts for everything. Macha's
current account of itself is `../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md`.

## Resume here (2026-11-16, in session)

**State.**
- `develop` is 0.9 (CMake 0.9.0, `libdannan0.9`), pushed at `ed5111d`. It holds reconfiguration and the fast path
  (merged at `208c581`), the advert push (`e78b66c`), and `main` at 0.8.5 merged back.
- **0.8.5 is the last release** (`main` and tag at `de631cc`, pushed at the maintainer's word; main's CI run 43800265662 green
  on all 15 jobs). 0.8.4 went just before it (tag at `52fd53c`, CI run 43793449409 green). Both releases' records are
  in `COMPLETED.md`.
- No branch is in flight.
- Next is step 2's remainder: value-less prepare and deltas.

**Lir's TSan report, open, and recurred on 0.8.5 with sharper frames** (Lir run 43802763961, tsan x86-64,
`ClusterTest.ARuleWrittenOnOneNodeAppliesOnEvery`; log `../lir/build/ci-logs/run-43802763961.log`).
- Both accesses are on one io thread (T1454), so this is no race:
  1. Lir's `SystemTopics::on_roster` (a `Nodes::watch` callback) takes `nodes().snapshot()`.
  2. It publishes: `ChannelsNoun::publish`, then `Fanout::publish`, then `Fanout::destinations` (`fanout.cpp:146`),
     whose roster copy's `~NodeRecord` frees a map node.
  3. Leaving the callback, `~RosterSnapshot` reads that freed node.
- Both copies are deep and taken under the members' lock, and Membership writes `_self` and `_nodes` only under
  it, so two fresh copies "sharing" a node means the allocator handed one block out twice.
- **Explained, by reading (2026-11-16): heap corruption from the ledger defect Lir suppresses.** At 0.8.5,
  `RetainedMemoryLedger::release` on a destroyed ledger erases from `_allocations` and `_owner_bytes`, whose nodes
  were freed with the ledger. That is a double free. TSan files a use-after-free under `race:` suppressions, and Lir
  suppresses `race:dannan::sched::RetainedMemoryLedger::release`, so the first bad write went unreported. The
  test before the failing one, `HeldMessagesAreLeasedInTheNodesLedger`, is the one that triggers it. Fixed on
  develop at `79dfa2e` (0.9 ABI).
- **Lir's checks on node-c (2026-11-16).**
  - ASan on 0.8.5: the defect fired in 1 run of 30 of `ClusterTest.*`. The report was a use-after-free read in
    `release` (`_allocations.find`), from a `Lease` in a fanout replay's completion, after `~LirNode` freed the
    ledger in `HeldMessagesAreLeasedInTheNodesLedger`'s teardown.
  - TSan with print_suppressions: 30 runs, no warnings, the ledger suppression never matched, and the NodeRecord
    report was not reproduced.
  - So the defect is confirmed, and the NodeRecord report is attributed to it by mechanism, not reproduced.
- **Lir waits for the 0.9.0 tag** rather than pinning develop (the maintainer, to Lir directly). It stays on 0.8.5 with the
  suppression until then, and will say at once if the NodeRecord report appears on a build with the fix. The
  one-thread reproduction is dropped unless it does.

**Lir's first TSan report against 0.8.3** (2026-11-16).
- What: a heap-use-after-free in `~NodeRecord`, in Lir run 43773090610 (`ClusterTest.ConnectingOnAnotherNodeTakesTheClientOver`, two
  nodes in one process, 4 io threads). The log is at `../lir/build/ci-logs/tsan-43773090610.log`.
  - The read: `~ClientHandshake` (`_local`, a copy of `Membership::self()`) under `~DialState` (`net/session.cpp:462`).
  - The earlier free, of the same map node: `~vector<NodeRecord>` of `observe_direct`'s `before` (`_members.active()`),
    from the server's established callback.
- Ruled out by reading: every Membership accessor and setter holds `_mutex`; `self()`, `active()` and `all()` return
  deep copies; ClientHandshake takes `local` by value; the dial and accept states share no record.
- Lir confirmed its `_deps` Dannan build uses lir_tests' flags (same g++ 14, C++20, no `_GLIBCXX_*` defines).
- Not reproduced in Dannan (2026-11-16):
  - `Node.NodesJoinWhileAdvertsChangeAndNothingIsShared` (`0358243`): 8 of 8 clean under clang 19 TSan (node-a), 8 of
    8 clean under g++ 14 TSan (node-c).
  - The whole suite (785 tests) clean under both.
- Not reproduced in Lir either (Lir, 2026-11-16, node-c, g++ 14 TSan at CI's flags, Dannan 0.8.3, Lir at `82352e6`):
  0 reports in 53 processes (the test alone 40 times, `ClusterTest.*` 10 times, the full suite 3 times).
- Lir keeps no record across threads. Its one reference handed to Dannan, the `const NodeRecord&` to
  `routes().call`, is copied at once: `place_call` takes it by value (`node.cpp:211-213`).
- **Status: open, unexplained, not suppressed.** Most likely the CI runner's timing, possibly a TSan artifact. If it
  recurs, a second report with other frames is what pins it; until then this stops being the first item.
- Lir also saw two other intermittent failures, being looked at on Lir's side: run 43757235866 (TSan) and run
  43765626713 (release arm64).

**Waiting on the maintainer:** nothing.

**A flake recurred, open:** `LevelZero.AChunkLostWithTheSendersSessionEndsTheCircuitWithinTheBound`
(`reach_paths_test.cpp:592`).
- It failed in develop's CI run 43900791401 (asan arm64, on `ba153ea`, a commit that touched only the plan), at
  lines 604, 605 and 607. After A closed its data route to the hub, the hub's circuit did not end within 20 s, B did
  not leave, and A counted no close with an unknown count.
- The next run, on `6ab20b8`, was green.
- It was seen once before, locally under clang at 0.5.x, and never in CI since.
- Not investigated yet. It is to be pinned (did the close reach a live route, and did the transfer end before it?)
  before anything is changed.

**Release mechanics learned 2026-11-16:**
- The `.deb`s CI builds before the tag exists come out versioned `<last>+gitN`. After pushing the tag, re-run
  `main`'s whole run (`gh run rerun <id>`; re-running a single deb job did not replace the arm64 artifact), then
  download that attempt's artifacts by id.
- Since 2026-11-16 every Pi runs a 4K-page 48-bit kernel, so a release's local gate is release, ASan and TSan again.
  On 4 GB node-d use -j1.

**Built for 0.9.0 so far** (see `COMPLETED.md`):
- Agreement: the CASPaxos core (`agree/caspaxos.h`), its wire (types 22 to 24, replies 110 to 112) and fuzz target,
  the runtime (`agree/agreement.h`), the node's wiring and `Context::agreement()`, `forget()` with tombstones and a
  per-space floor, and `catch_up()` for a store that lost its state.
- The simulation harness, phase one (`test/fixtures/sim/`): five real nodes stay linearizable under lost, delayed,
  duplicated and one-way messages and crash-restarts. The broken protocol (voting after losing state) is caught, and
  catch-up holds while a minority is wiped.

**0.9.0 is cut when the whole plan below is done** (the maintainer, 2026-11-16, asked: not before, and not after agreement
alone). Asked again later that day, he kept it: "I'd rather do it right - but it is becoming a chokepoint, we've been
at this some time. Complete it properly." Lir, Mimir and Kvasir wait on the tag, so keep each step moving: no
gold-plating beyond what the design and its reviews ask.

**Next, in order** (the maintainer: agreement first, half the stack waits on it):
1. **Reconfiguration, as the maintainer ruled** (2026-11-15: "No root except at genesis. A root may start with only one
   node."). The design is `2026-11-15-agreement-design.md`, "Reconfiguration", agreed by Mimir and Kvasir:
   - **Merged** to develop at `208c581` after the branch's CI (run 43786450604, 15 of 15).
   - **Built** on `feature/agreement-reconfiguration` (`2329b9f`), with the gap's fix: per-slot records, `transfer()`,
     settle, `found()`, the proof rule, the laggard rule. The table is `NodeConfig::agreement_slots` and
     `agreement_genesis` (genesis at version 0).
   - **Built** at `c524a28`: every request carries the table's fingerprint and an acceptor with another table refuses
     it; the slot advert (`agree.slots/<space>`) and `Agreement::hint()`. The suite on node-d ran 778 tests: 777
     passed and 1 skipped (it is skipped when run as root).
   - **Refused at join as well** (the maintainer, 2026-11-16, asked: "Also refuse at join"). **Built and merged** at `a521260`.
     - Each node publishes its table's fingerprint as the `agree.table` advert on its record, which the handshake
       already carries both ways, so no wire field was added.
     - `Credentials::admit` checks an authenticated peer's record on both sides. The node refuses a differing
       fingerprint, and the refusing node's journal names the table.
     - A peer that publishes none (an older node) joins, and is refused at every agreement request.
     - Tests: `Channel.APeerNotAdmittedByItsRecordIsRefusedBothWays`, and
       `Node.ANodeWithAnotherSlotTableIsRefusedAtJoin` (another count, and another genesis set).
     - The suite on node-c ran 825 tests: 824 passed and 1 skipped (it is skipped when run as root).
   - The test "the fast path lost across a transfer" is `FastPath.IsLostAcrossATransfer` (`6c90f50`).
2. **The fast path: built** at `6c90f50`.
   - How it works: a plain accept reserves the proposer's next ballot (`Accept::reserve`), and the next change of the
     same names is an accept at that ballot with `fast_from`. A change with an owner's reduction never goes fast (merge
     writes are seen only in a prepare). The runtime caches the last commit per space, slot, set and names; a miss
     goes the full way at once.
   - What the simulator found on the way: a fast ballot chosen at send time could outrank a commit made in between;
     reserving it is CASPaxos's own form, and fixes it. A retry after a partial accept applied its change twice; a
     retry that finds its own accept newest now commits it unchanged.
   - The suite on node-d ran 784 tests: 783 passed and 1 skipped (it is skipped when run as root).
   - Still to do in step 2: **value-less prepare and deltas**. Designed and reviewed by Mimir and Kvasir
     (`2026-11-15-agreement-design.md`, "Value-less prepare and deltas", at `82c18a8`). Build order, each with its
     tests first and its own commit:
     1. The summary (per name, 64 bytes, set by the store) and the has-value bit in `AcceptorState` and `NameState`;
        a "values wanted" flag on prepare; `load()` leaving values empty when value-less. **Built** on
        `feature/valueless-prepare` at `c71eb20`. `keep()`'s contract now says a state held but not carried keeps the
        store's value and summary, which part 4's in-place apply needs too. Every attempt still asks for values. The
        suite on node-b ran 799 tests: 798 passed and 1 skipped (it is skipped when run as root).
     2. The fetch: `agree_fetch` by name, ballot, offset and `since_summary`, answered in pieces; the store's
        `value(...)`. Then the Attempt's fetching phase between promise and accept, taking the base from this node's
        own acceptor when it holds it. **Built** at `acaab48`.
        - The pieces are at most 1 MiB (`max_piece`) and are resumed by offset. A fetch moves between holders of
          the ballot from the same offset.
        - `value()`'s default loads the name.
        - The runtime asks every plain change without a reduction for no values.
        - `since_summary` waits for part 3, where Mimir's reduction needs it. The first cut fetches whole by ballot,
          as Mimir allowed.
        - The suite on node-c ran 806 tests: 805 passed and 1 skipped (it is skipped when run as root).
        - Hosted CI runs on develop only, so the branch's sanitizer runs come at its merge.
     3. The reduction on summaries (Mimir's (a)): it returns its result or the names it needs fetched. **Built** at
        `5a09792`.
        - `Reduce` returns `Reduced{value, need}`. A need is an answer, whole or past a `since` summary. It is
          fetched by that answer's ballot and summary, and the reduction is called again. Asking twice for one
          answer refuses the attempt.
        - Holders match on ballot and summary, since merge writes leave one ballot with different values.
        - A change with a reduction goes value-less too; a transfer still carries values.
        - The fetch and `value()` carry `since`; the default `value()` answers whole.
        - Mimir was told of the API change.
        - The suite on node-a ran 809 tests: 808 passed and 1 skipped (it is skipped when run as root).
     4. Deltas: a change may return a delta per name. The accept carries, per name, a whole value, a delta with its
        base ballot, or the ballot only (Kvasir's "already held"). The store's in-place `apply(slot, name,
        base_ballot, new_ballot, delta, done(applied | need_whole))`. A "need whole" is resent in pieces.
        **Built** at `3d771a1`, with the delta inside `keep()`, not a separate `apply()`. A separate call could not be
        atomic. the maintainer asked for review first, and Mimir and Kvasir took it; the contract is in the design document.
        - A delta goes only to an acceptor that answered holding its base (ballot and summary).
        - The rule asks for a name whole off the base, and answers a replay at the new ballot without the delta.
        - `keep()` answers `Kept{kept, need_whole}`, all or nothing.
        - The proposer resends names whole to an acceptor that asks.
        - The `past` flag on a fetch past `since` is in (`0275e67`, Mimir's ask).
        - **Still to do:** a whole resend larger than one message, in pieces; the "ballot only" accept. Both belong
          with `transfer()` in part 5. The fast path sends whole values.
        - The suite on node-a ran 813 tests: 812 passed and 1 skipped (it is skipped when run as root).
     5. `transfer()` over all of it, then the simulator with large values and deltas under faults. **Built** at
        `1c624d3`.
        - A transfer is value-less, its bases fetched from holders in either set.
        - An accept's third form is the ballot only (Kvasir's "already held"), for a value left as it is.
        - Every compact form carries its base's ballot and summary, and the rule asks whole where either differs.
        - An acceptor not heard from is sent the compact form and asks for what it lacks.
        - Tests:
          - `Held.*`;
          - `Simulation.ATransferWhoseNewAcceptorsHoldTheValuesSendsNoValue` (every accept under 64 KiB);
          - `Simulation.LargeValuesAndDeltasStayConsistentUnderFaults` (8 seeds; drops, duplicates, delays and a
            crash; every append applied once).
        - The suite on node-a ran 817 tests: 816 passed and 1 skipped (it is skipped when run as root).
        - The store interface is final; Mimir and Kvasir were told.
        - Mimir accepted the store side as final (2026-11-16).
        - Kvasir accepted the store side too. **A whole resend in pieces is not needed** (Kvasir): ranges stay below
          `max_value` by Kvasir's own split, and a lagging acceptor is caught up by the store before it is sent
          anything.
     6. **The value-less change** (Mimir's and Kvasir's ask; the maintainer, 2026-11-16: "Include in 0.9.0", since under the
        ABI rule it would otherwise wait for 0.10.0). Every write to a large collection or range otherwise copies the
        whole value at the proposer. Kvasir commits every 10 to 20 ms.
        - `change_deltas(...)`. The change is given each name's base (ballot, summary, has-value) and whether this
          node's own acceptor holds exactly it. It returns a delta per name (none: unchanged) and a result.
        - No fetch and no whole value anywhere. Holders get the delta, or the ballot only for an unchanged name.
        - A majority must take it. An acceptor needing the whole value is not counted and gets no resend; the
          Outcome names it, for the owner to catch up.
        - The fast path keeps ballots and summaries, not bytes. A transfer and catch_up() are unchanged.
        - The API went to Mimir and Kvasir before it was built. Kvasir agreed it as written (2026-11-16):
          - the fast path checking by ballot only is right for ranges;
          - need_whole nodes are reported, not resent;
          - `local` false may return no deltas.
        - Kvasir's read: a `change_deltas` with no delta for any name commits without an accept when every answer
          in the promising majority names the same ballot and summary. That state is already chosen, so a current
          read is one round and no sync. Otherwise it falls back to the ballot-only accept. The proof rule applies.
        - Mimir's review, taken (2026-11-16):
          - `Deltas::decline` ends the change at once (`why = "declined"`), for a base not local or moved under
            Mimir's lock.
          - `Deltas::summaries`: per name, the summary keep() will set. The fast path keeps ballot and summary and
            checks both, and a name with none takes no fast path. Commutation is not relied on: a reply computed
            from a merged-over base would be wrong.
          - Contract: DeltaChange runs on the agreement's strand, never under a Dannan lock or waiting on a store
            completion. need_whole acceptors are caught up by an identity change(), and they block nothing beyond
            liveness.
        - Kvasir was told of the summary rule; it can supply summaries and keep the fast path.
        - The read's promises are kept like any prepare's (Kvasir's option (a)): one round and one sync per promiser.
          **After 0.9.0:** a read-only prepare that is not kept, returning only when all answers match and otherwise
          starting again with a kept prepare. Option (b) as first stated is unsafe through its fallback accept. It
          needs Kvasir's simulator run: crashes between the read's promise and a lower-ballot accept.
        - **Built** at `c23086a`.
          - Tests: `ValueLessChange.*`, and `Simulation.AValueLessChangeCarriesOnlyDeltas`. The simulation covers a
            megabyte delta with no fetch, the fast path, a read with no accept, need_whole named then caught up by
            an identity change, and a decline.
          - The suite on node-d ran 823 tests: 822 passed and 1 skipped (it is skipped when run as root).
        - **Merged** to develop at `6686731`. Develop's CI run 43938400849 was green on every Linux job: release,
          ASan and TSan on x86-64, ASan on arm64, clang, coverage, abi, deb and package. The macOS job was cancelled
          after 15 minutes having run no step. It did the same on its rerun and on the next run (43941344441), so no
          macOS runner is taking jobs. the maintainer: "There isn't a macos runner", "You use this mac". The hosted job was
          removed (`9f1bce2`). The macOS gate now runs on the maintainer's Mac at each merge to develop and before a release,
          against MacPorts `boost188` (installed 2026-11-16), with `-DBoost_ROOT=/opt/local/libexec/boost/1.88`.
          - First run, develop at `9f1bce2`: 824 tests, 822 passed and 2 skipped, both Linux-only:
            `CustodyTool.InitTakesCustodyBackWhenTheRootCannotBeWritten` and `PeerSocket.ChoosesBbrWhereOffered`. Today a whole value in an accept
          is one message, up to `max_value`. Once he answers, the branch merges to develop.

   Tests are named there: one-node genesis grown to five, the root unreachable, the fast path lost across a
   transfer.
   Value-less prepare and deltas come before a transfer of large values (Kvasir's ranges: new acceptors are caught up
   by the store first, and the joint round then carries a version or a delta).
3. **The harness's next phase** (`2026-11-15-simulation-harness.md`):
   - membership, Routes, placement and correlated kills in the world;
   - the history with faults and resolutions;
   - the checkers and the broken-protocol list;
   - `dannan_sim_checkers`.

   Phase one is built already: the world, its generator, `SimTime`, and a cluster with message faults and store
   crashes, wipes and lying syncs. Agreement runs on it with a counter-only check. In order, each with its tests and
   commit (2026-11-16):
   1. **The history and the linearizability checker.** `history.h` holds operations with an id, client, keys,
      invocation, response (ok, none or cancelled), optional resolution, and the faults as events.
      `dannan_sim_checkers` is a static library holding the Wing-Gong search over owner-supplied models: state as
      bytes; a model answers the allowed next states for an operation and its result, more than one allowed; groups
      of keys are split by the operations that touch them; an operation with no response may or may not take
      effect. The counter simulation moves onto it.
   2. **Agreement's own invariants on every step**, and the broken protocols, each refused with its seed:
      - voting after losing state (exists);
      - a proposer skipping prepare;
      - an acceptor accepting below its promise;
      - a reducer rolling the base back;
      - a lying sync trusted;
      - two proposers with different rosters naming different sets;
      - a reducer dropping a `replicated` merge write.
   3. **Correlated kills** (a majority at one step), and the durability rules per `ack_level` judged against the
      faults in the history.
   4. **Membership, Routes and placement in the world.** This needs a transport seam under the node. A design note
      goes to Kvasir and Mimir first.
   5. **The transaction checker** (Adya cycles by isolation level, Kvasir's model), the convergence checker
      (Mimir's merge class), client actors, and the worker pool in the world.
   6. **`SimDisk`** on the durable-file contract, once Dagda says which one (asked; still open).
4. **The file-system interface** (`2026-11-15-file-system-interface.md`, Dagda's review taken).
5. **The hybrid clock** (`2026-11-15-hlc-design.md`, reviewed).
6. **The rest of 0.9.0:**
   - the per-call record copies (carry `shared_ptr<const NodeRecord>`; Dagda's profile);
   - DTLS and UDP for `dannan::listen`;
   - the peer-witnessed high-water mark;
   - loader promotion at the peer.

   The wire additions are designed and reviewed first.
7. Atomic commit across replica sets. Mimir needs it for ruling 22's multi-slot commands now that a change names one
   slot. Mimir's proposal is `../mimir/TODO/2026-11-15-cross-slot-commit-proposal.md` (Proposed, not yet put to the maintainer):
   - `transact(space, parts, f, deadline)`, with intents inside register values and one transaction-record register
     as the commit point;
   - older wins;
   - three questions for Dannan at its end.

   Kvasir's corrections (2026-11-16):
   - commits of known values (its buffered SQL writes);
   - intents at the hosted system's granularity, laid, found and resolved by the hosted system, with Dannan owning
     the record, the decide and the finishing;
   - a pluggable conflict rule (the maintainer ruled that a decided or replicated commit wins over an undecided one);
   - a commit stamp by the hybrid clock at decide.
9. **Built and merged** at `e78b66c` (787 tests on node-c: 786 passed, 1 skipped). Two Node tests run with a 30 s
   heartbeat: `APushedAdvertIsHeldByEveryPeerBeforeItIsDone` and `ASubscriptionHeldBeforeItsDoneGetsTheNextPublish`.
   It also serialises readvertise: two racing subscribes could leave the older prefix list advertised. Lir confirmed it
   on a local branch: it holds SUBACK on a 1 s deadline, and `SlowGossipClusterTest.AMessagePublishedElsewhereRight
   AfterSubackArrives` passed 20 of 20, with its suite at 355 of 355. No API change asked; it merges at 0.9.0.
   **Ruled (the maintainer, 2026-11-16, relayed by Lir): both calls below in 0.9, no 0.8.x stopgap.** Lir holds a new prefix's
   SUBACK until the advert is held or the deadline passes. On a miss it grants the subscription and logs the peers that
   missed it. The ask: push a changed advert and learn who holds it.
   - The finding: on Lir's cluster a QoS 1 publish right after a SUBACK on another node is lost for that subscriber.
     It was delivered 0 of 20 times at 0 ms and 8 of 8 at 5 s. The cause is the interest advert travelling at heartbeat
     pace (`net/fanout.h:29`), and nothing records the gap.
   - Recommended:
     - `Nodes::advertise(key, value, deadline, executor, done)`, which sends the changed self record over the members
       exchange (type 2) to every active peer and reports held and missed;
     - `Fanout::subscribe(..., deadline, executor, done)` on top of it.
   - A virtual interface addition, so 0.9.
10. **Released in 0.8.5** (the maintainer chose a patch). **Lugh's sign-in Backoff into Dannan** (Kvasir's ask, 2026-11-16,
    stack rule 3; agreed). It becomes
    `dannan::listen::Backoff` (`listen/backoff.h`), Lugh's class and contract as they stand, on
    `contract::TimePoint`, per node. Lugh's tests move with it (`lugh/test/authn_backoff_test.cpp`, suite Backoff:
    FreeFailuresThenDoublingWaits, TheWaitHasACeiling, KeysAreSeparateAndSuccessClearsOne, QuietKeysAreForgotten,
    MadeUpKeysDoNotGrowWithoutBound). Once it is tagged, Lugh switches its Authenticator and deletes its copy and
    tests; the cluster-wide sum stays Lugh's (P8). Kvasir's MySQL sign-in keys it per principal and per source.
    Small: right after item 9. Header-only and additive, so it could also go out in an 0.8.x patch: the maintainer's call.
8. **Export the fuzz harness** (Kvasir's ask, 2026-11-15; Mimir wants it too), so hosted systems link it rather than
   copy it (the stack's rule 3): `fuzz/fuzz.h`'s Target and Registration, `targets()`, the replay driver, and
   `fuzz_smoke_test.cpp`'s mutate and seeds-then-mutations-then-noise loop as a callable, installed header-only with
   the test fixtures. A hosted system then runs its targets in its own test binary with no clang, which no build Pi
   has.

**Standing, from the maintainer's rulings (2026-11-15):**
- Anything that can use agreement waits for it: no stack project builds its own consensus meanwhile.
- Lugh's TOTP, recovery codes and key rotation use agreement registers on 0.9.0 (relayed by Lugh).
- The `ack_level` names are standard: ephemeral, persisted, decided, replicated (relayed by Kvasir).
- The sim harness is built once, in Dannan (relayed by Kvasir).

**Peers' open threads:** Mimir and Kvasir are building on the slot table. Lir was told 0.7.0 and 0.8.3 interoperate,
so it may roll its upgrade one node at a time.

**Where things are built and tested** (the maintainer, 2026-11-14: "STOP all builds on the Mac now. Use the RPi cluster only."):
- Never build or run tests on the Mac. Editing, git, gh, reading CI logs and clang-format are fine. The one exception
  (the maintainer, 2026-11-16, told to every session): "TSan may be run on the mac IF AND ONLY IF bughunting." Since 2026-11-16
  every Pi boots the 4K-page kernel (the maintainer had the cluster manager switch them), so g++ TSan runs on all four and clang
  19 TSan on all but node-d. No sanitizer builds on node-d: it has 4 GB, and Dannan's ASan build hung it. Use node-b or
  node-c, after `free -m`.
- Hosted CI (`.github/workflows/ci.yml`) is the gate: 15 jobs on Debian x86-64 and arm64 and macOS, with `abi`
  against the minor's last tag. To run it on a feature branch, add a temporary commit putting the branch in
  `on.push.branches`, and drop it before the merge with `git rebase --onto X^ X`. Its logs come down with
  `curl` and `gh auth token` into `build/ci/` (`gh run view --log` waits for the whole run). CodeQL is a separate run
  on the same commit: don't take it for the tests.
- The Pi 5 cluster: node-a, -node-b, -node-c and -node-d, reached as `ssh root@<ip>`. Claim cores on the
  claim board first and release them when the job ends. Use nice 19 and a modest `-j`.
- live-a and live-b, which run Macha, still need the maintainer's word.

**Release rules:**
- Tags need the maintainer's direct word.
- The version commit changes CMake, README, CHANGELOG, COMPLETED and ACTIVE, and `debian/control`'s library package
  when a minor starts.
- Within a minor the ABI is kept, and `tools/ci.sh abi` enforces it. An interface change waits for the next minor.

## The one rule

**Macha is ported onto Dannan and Dagda with no loss of function.** Not
"no regression a test catches": every behaviour Macha has today, whether
a test names it or not, is still there once it runs on the two libraries,
and a Macha user, operator or client cannot tell. "Unchanged" is defined,
not felt: the correctness gates in `macha/VALIDATION.md`, the crash matrix
in `macha/docs/durability.md`, every route in `macha/docs/api.md` and
every contract in `macha/TODO/CLIENT-CONTRACTS.md`, the config schema as
`macha.yaml.example` and the config golden define it, every field of
`GET /api/v1/status` and its sub-resources (the Status golden), the FUSE
behaviour, the tools, the plugin loading and build-identity refusal,
`build/deploy.sh` and the rollback directories, protocol 23 and layout v18
until level 0. The inventory lists the rest, with how each is proven, and
a port step that cannot name the proof for a behaviour it touches does
not happen.

Two consequences that shape every step:

- **Dannan is written from the design, not transcribed from Macha.** Its
  shape, names, namespaces, style and tests are its own from the first
  file. Macha's code is read for what it does, where the inventory points,
  and never copied; a Dannan test comes from the design, the laws and the
  inventory row, and is written before the code. Where the design's form
  and Macha's behaviour differ, the behaviour is what the port keeps
  (grandfathering) and the design's form arrives at the protocol bump the
  design names for it.
- **No byte moves.** Nothing on disk or on the wire changes before level
  0: the handshake and protocol 23, the frame, the fragments, the message
  numbers, the roster and telemetry records, the metadata snapshot
  (SM15 to SM18, DLT9/DLT10), layout v18. Dannan speaks Macha's wire
  exactly, so a Dannan node and a Macha node form one cluster, which the
  two-process test proves at every step that touches the wire. The plan
  has exactly two protocol bumps: 24 for level 0 and 25 for identity.

## Waiting on the maintainer

Each stops something; a session that has lost its context sees them
first.

Rulings on the first cutover, the maintainer, 2026-11-05, settled:

- **A. Deadlines (F2).** "Every call must specify, there is no default. Macha will have to
  adapt to that in the refactor." `Routes::call` keeps its required deadline, there is no
  blanket constant in the facade, and each of Macha's call sites chooses one in the port.
- **B. Thread rows (F14).** Accepted: `/status/diagnostics` changes bytes, rows with no
  thread go, and the three loop threads are renamed `cluster-telemetry`,
  `cluster-maintenance` and `cluster-connectivity` (step 6).
- **C. DIAG lines (G48).** Dannan adds a thread CPU reporter, per pool on the worker and io
  threads, not per connection (step 5).
- **D. Bounds (replaces the first answer).** "the bounds exist to enforce fair usage and
  prevent problems. For Dannan, the concept is more useful than strict fidelity with Macha -
  BUT, the existing macha budgets and limits MUST be representable in Dannan IF Dannan is
  responsible for that layer or how that layer is implemented." So limits above the layer
  (playback sessions, transcodes, password checks) stay Macha's, and Dannan adds no
  per-principal bound for them. Limits in the layer Dannan owns must each be expressible in
  Dannan, not necessarily in Macha's form (step 8's budget table).
- **Disk limits are Dagda's; the generics are shared.** the maintainer, 2026-11-05: Dagda owns disk
  limits but should use the same work-class generics for them: "common code goes to the
  lowest level, Dagda depends on Dannan". Dagda already uses `WorkClass` and `WorkContext`.
  Under discussion with the Macha session: `sched::RetainedMemoryLedger`, Dagda's
  `DataResourceArbiter` and its `IoReserve` look like one admission mechanism written three
  times, with different policies. That Dagda is mid-work does not hold this back; Dagda
  takes direction from this session.
- **Live nodes.** The Macha session's own loopback runs on live-a were fine. The hold
  covers Dannan's work, the Pi measurements and the gateway.
- **The live-a copy.** Allowed, read-only, without stopping the node: the roster, inbound
  and telemetry files only (step 7).
- **The Pi baselines.** The Macha session's, later, at his word, with the deployed version.

Held until he says: the Pi baselines and measurements, the real IGD run, anything on
live-a or live-b. Each push and release is his word too.

0. the maintainer, 2026-11-04 (after 0.3.0): the Macha port (steps 3, 6, 8) **not
   yet**; the AthenaSIP port (step 11) **not yet**, until Dagda's heads and
   leases have a tagged release. Meanwhile Dannan serves Dagda, Lugh and
   Lir. Pushed at the maintainer's word the same day: `origin` is
   `git@github.com:tomdionysus/dannan.git`, holding main, develop and the
   tags 0.1.0, 0.2.0, 0.3.0 (0.4.0 pushed at his word too). Each later
   push still needs his ask.

1. **The suite counts on the live nodes** at `0.91.2`, and at each Macha release
   the port makes, recorded in `COMPLETED.md`. A deploy-day act under
   Macha's rules.
2. **Cloud providers and budget for the chaos cluster**, when level 0 is
   ready to be tried for real. Until then: the compose cluster only (the maintainer,
   2026-11-03), and no cloud spend.
3. **Macha findings that are Macha's to settle**, noted here only:
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`
   is flaky on macOS at the baseline (1 run in 5), and
   `maintenance_trace/test_trace_backend_offline_and_back` on live-a
   (Macha session, 1 in 3). Both are with the Macha session.
4. Settled: the first commit is `2a923f5` on `main`; `develop` branched.
   `CLAUDE.md` stays uncommitted (the maintainer's global gitignore excludes it).
   the maintainer (2026-11-03): commit at each completed milestone without asking;
   no attribution lines, ever.
5. Settled: the maintainer had the Macha session delete the morning's leftovers in
   `../macha` (the goldens branch, the two-process script, the 0.91.2
   worktree and its runs). The goldens are rewritten at step 3 as part of
   the port gate; the two-process test is written here at step 2.
6. Settled: GoogleTest 1.18.0 from MacPorts (`gtest`), the only one on
   the laptop.
7. Settled (2026-11-04): the repository copies are the masters, so the
   design's "starts in-tree" paragraph was corrected here to the maintainer's
   decision that Dannan is its own repository.
8. Settled (the maintainer, 2026-11-03): core message types are Dannan's; the rest
   are user-definable and under the user's control. See Decisions.

9. Settled (step 10, 2026-11-04): a node is revoked at once by a
   CA-signed revocation, gossiped and enforced at every v2 handshake; the
   Context stays callback style (Lugh decided). Was: **Revoking a single
   node at once** (Lugh, 2026-11-03 night): Lugh needs
   a removed node refused immediately. Step 10's design (cluster CA, node
   certificates) says a compromised node is cut off by rotation; immediate
   per-node revocation is not designed. Also asked: whether the Context
   offers completion tokens or awaitables beside callbacks (Lugh is built
   on asio::awaitable).

## How to prove it

Two gates, and both are run.

**Dannan's own gate**, every commit here: `dannan_tests` and
`dagda_tests` (`-DDANNAN_BUILD_TESTING=ON`, run the binaries), counts
recorded in `COMPLETED.md` and never lower; a test is never deleted; time
is injected and nothing sleeps; sanitizers before a release and to hunt a
bug that cannot be pinned any other way (`build-asan/` with
`ASAN_OPTIONS=detect_container_overflow=0`, `build-tsan/` with
`TSAN_OPTIONS=suppressions=$PWD/dannan/test/tsan-suppressions.txt`; the
flags are in `COMPLETED.md`'s 0.1.0 entries). At every step that speaks the
wire, the two-process test: a Dannan node (a small executable built here)
against the Macha binary over loopback, both ways round, in whatever form
the step can prove (step 2: handshake, membership, telemetry, both lanes;
step 7: objects, commits both ways, a divergence merged).

**The port gate**, before and after every port step in `../macha`, on
the laptop and then on the live nodes: the nine items of "The seam gate" in the
extraction document, which are: the three suites with counts equal or
above, the config golden, the Status golden, the route list, the plugins,
the install, wire and disk (protocol 23, layout v18, the mixed-build
two-process test), the diff reviewed with every content change named,
and the inventory updated. Macha's rules on the laptop hold: no verdict on
timing under load, touched tests here, all three suites on the live nodes, never
both nodes down at once.

```
Macha:  cmake -S . -B build-macports ... (COMPLETED.md has the line)
        cmake --build build-macports -j8
        sh ./run-tests.sh build-macports
Dannan: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TESTING=ON \
          -DDANNAN_WARNINGS_AS_ERRORS=ON \
          -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local" \
          -DDANNAN_MACHA_INTEROP_DIR=$PWD/build/interop/macha-0.91.2 \
          -DDANNAN_MACHA_BINARY=$PWD/../macha/build-macports/macha
        cmake --build build -j8
        ./build/dannan_tests && ./build/dagda_tests && ./build/dannan_interop_tests
```

`dannan_interop_tests` holds the in-process byte-for-byte cases (against
the pinned 0.91.2 export) and, with `DANNAN_MACHA_BINARY`, the two-process
test (that binary and `dannan-node`, about 5 s).

For AthenaSIP (step 11): `./build-tests/athenasip_tests` with Redis and
MQTT set, then `test/e2e/cluster.sh` with the second node behind CGNAT and
no public address (`docs/testing.md` there is the whole of it).

## Principles

1. **Dannan is the network layer, the virtual PHY and MAC.** It owns
   reachability, lanes and channels, membership and capabilities, node
   identity and scope, placement as a pure function, work classes and
   budgets, supervision, time and status. It stores nothing for anyone.
2. **Dagda is a separate library and a peer**, one hosted system among
   the others, not a floor under them: objects, claims, GC, the trie,
   opaque heads, leases, domains. A node may run Dannan alone.
3. **The membership test**: a thing is Dannan only if every hosted system
   needs it identically and none needs it in its own shape. What fails
   stays out permanently.
4. **Hosted systems never address one another**; they share objects, heads
   and channels. HTTP and client protocols are theirs.
5. **The four laws and five disciplines** (`docs/principles-and-laws.md`
   here, a verbatim copy of Macha's) apply, cited by number, to every
   hosted system. "Viewer" is a hosted-system word; Dannan says
   foreground.
6. **Every call carries a work class, a deadline and a principal, and
   returns evidence**, never a boolean; no call waits without a deadline;
   every completion is posted to the caller's executor, never run inline.
7. **Limits are boundaries, not gaps**: no consensus, quorum, leader,
   lock, transaction, uniqueness guarantee, global clock, exactly-once
   delivery or cross-origin ordering; trusted non-Byzantine peers only.
8. **Written here from the design; proven on Macha first, then
   AthenaSIP**, which pulls from the opposite side; the door is right when
   the next hosted system needs no core edit. A boundary is pulled on from
   both sides as early as the plan allows: Macha links Dannan's transport
   (step 3) before the rest of Dannan exists.

## Decisions

Dated, from the design documents unless marked here, and not reopened
without asking. A superseded decision stays listed, struck through in
words, so a session does not re-derive it.

- (2026-11-02, design) **Name: Dannan.** Two libraries: Dannan (network)
  and Dagda (the store). Dagda links Dannan and is a hosted system.
- (2026-11-02, design; **superseded 2026-11-03 evening, the maintainer**) ~~In-tree
  first: Dannan starts in Macha as a CMake target and becomes its own
  repository only after AthenaSIP runs on it.~~ Replaced by **Here
  first**: Dannan and Dagda are written in this repository from the
  design, and Macha is ported onto them piece by piece, each piece
  linked as a tag of this repository. The boundary is still pulled on
  from two sides: Macha at every port step, AthenaSIP at step 11.
- (2026-11-02, design) **Harness before seams** becomes: Dannan's own
  test fixtures (the in-process cluster, the gate, the stepped clock)
  are step 0; the compose cluster gates level 0; the local suites gate
  every commit.
- (2026-11-02, design) **Strangler order, cheapest first, the snapshot
  last**, now as port steps: transport, then the control plane, then the
  store, the opaque heads last; one step per session and branch; gates
  green before and after.
- (2026-11-02, design) **Grandfathering.** The FS owner's codec for its
  opaque head state is the existing SM15 to SM18 and DLT9/DLT10
  encodings, so Dagda's generic head is byte-compatible with 0.91 by
  construction. The first generic format is FS-shaped and that is
  accepted; a layout bump (v18 to v19) and protocol bump (23 to 24) with
  a `macha-namespace-migrate`-style one-shot are reserved for the first
  state the FS encoding cannot carry.
- (2026-11-02, design) **Head stays in the core**: Dagda knows what
  divergence is and calls the owner's merge; it never merges itself.
- (2026-11-02, design) **Identity is the certificate, never the address.**
  AthenaSIP's cluster CA (EC P-256, two-year node certs with SANs, mutual
  TLS, `ca.key` offline) is the model; Macha's single shared key is the
  degenerate case, a CA with one node class. Users and services are Lugh
  principals. Until protocol 25 Dannan's identity is the shared cluster
  key, exactly as Macha's handshake uses it.
- (2026-11-02, design) **Four channel contracts and no more**: notify,
  stream, bulk, retained. Subscription is node-local; fan-out follows
  gossip. The event system's broker is built on top, not in.
- (2026-11-02, design) **CONTROL relay is unconditional, DATA relay is
  budgeted**; a path change is a reconnect under the same identity.
- (2026-11-02, design) **Dannan has no namespace and no resolver**; bind9
  serves; a zone is a store head with a DNS-tolerant merge.
- (2026-11-02, design) **The hosted-system ABI is the C++ ABI with a build
  identity check**, as both repositories do today. No C ABI is promised.
- (2026-11-02, design) **One wire protocol number for the whole cluster**,
  owned by Dannan; mismatch fails the handshake; fresh-genesis state.
- (2026-11-03, this plan) **HTTP route registration is Macha-internal, off
  the critical path.** `HttpServer` and its route classifier stay in
  Macha; the seam is kept in Parked for when two hosted systems share one
  listener.
- (2026-11-03, this plan; **superseded 2026-11-03 evening**) ~~Two seams
  the designs assume but do not name come first: the config split and the
  `NodeRuntime` split.~~ They are no longer prerequisites for anything
  here: Dannan's transport takes its parameters by constructor, as
  Macha's does today, so Macha links it (step 3) with `Config` and
  `NodeRuntime` as they are. Both splits are Macha-side work at step 6,
  when Macha's control plane becomes Dannan's node, and their text in the
  extraction document (seams 1 and 2) still says what they are.
- (2026-11-03, this plan; **superseded 2026-11-03 evening**) ~~Namespaces
  stay `macha` in-tree; the rename and the reformat are one mechanical
  commit at the split.~~ There is no split. Namespaces are
  `dannan::<subsystem>` and `dagda::<subsystem>` and the style is
  Dannan's from the first file.
- (2026-11-03, this plan; **superseded 2026-11-03 evening**) ~~The route
  table must widen before it opens.~~ Dannan's route table is open from
  the first commit: a 16-bit type number, registered with lane, class,
  whether it is a notification and its handler; Dannan reserves its own
  numbers (the extraction document, seam 4, lists them); Macha's table is
  not widened, it is replaced at step 3 by registrations of Macha's
  numbers.
- (2026-11-03, this plan; **superseded 2026-11-03 evening**) ~~Macha's
  own test runner carries the moved tests through the split.~~ No test
  moves. Dannan's tests are GoogleTest from the first file, written from
  the design and the inventory; Macha's tests stay Macha's and are the
  port gate. `TestCluster`, `TestNode`, `TestGate` and `SteppedTime` are
  the model for Dannan's fixtures (`test/`), written fresh.
- (2026-11-03, the maintainer) **Status is a Dannan noun.** One Status surface per
  node: `publish(key, value)` and `counter(name)` on the context, every
  hosted system publishing into it, the parked state and the counters of
  Dannan itself beside them. Macha's `GET /api/v1/status` reads Dannan's
  surface plus its own and stays byte-identical (the Status golden);
  AthenaSIP's `GET /api/v1/nodes` reads the same surface. The telemetry
  record's generic shape at protocol 25 is this surface's gossip.
- (2026-11-03, the maintainer) **The laptop is MacPorts for everything.** Done.
- (2026-11-03, the maintainer) **Extraction proceeds on the laptop now.** The pause
  on Macha development does not cover it; deployment to the live nodes is the gate
  it already is, taken when convenient.
- (2026-11-03, the maintainer) **The store is Dagda.** Target `dagda`, directory
  `dagda/`, namespace `dagda`. The Dagda's cauldron never runs empty.
- (2026-11-03, the maintainer; applied from the first file, not at a split)
  **AthenaSIP's mechanics are Dannan's style**: Google base, 160 columns,
  two-space indent, `.h`, the header block, `_name` members,
  `dannan::<subsystem>` namespaces. Macha itself stays in its own style.
- (2026-11-03, the maintainer) **Boost.Asio from the start.** The transport is
  Dannan's first substantial piece (step 2), on Asio, speaking protocol
  23 byte for byte, with every transport behaviour in the inventory kept
  and the executor rule native. Macha replaces `cluster/net.cpp` with it
  at step 3, before anything else of Dannan exists, because the thread
  model is what the single live node exercises hardest.
- (2026-11-03, the maintainer) **Head spaces carry a replica policy from the start.**
  `replicas: N` is accepted at `open()` in v1, with CONTROL placement per
  space and forwarded reads over CONTROL that say so in the evidence.
  Macha's own space is opened every-node.
- (2026-11-03, the maintainer) **Scope is looked up in the Lugh head at write time**,
  by every replica that counts toward the floor; a node that has lost the
  Lugh space accepts a short-lived credential's scope instead.
- (2026-11-03, the maintainer) **TCP simultaneous open first; UDP later.** Level 0
  ships with TCP punching and relay; the lanes, `SecureChannel`, fragments
  and BBR are untouched. A UDP lane is a later step.
- (2026-11-03, the maintainer) **Two protocol bumps**: 24 for level 0 (path classes,
  relay, the punch broker's messages), 25 for identity (certificates in
  the handshake). Level 0 is proven on a cluster before the handshake
  changes.
- (2026-11-03, the maintainer) **The compose cluster only, for now.** One machine,
  docker networks for NAT, a scenario runner for terminate, partition and
  clock skew. No cloud spend until level 0 is ready to be tried for real.
- (2026-11-03, the maintainer) **Write every owed transport proof before the
  transport is rewritten.** Done in Macha (21 cases, `c71370e`); they are
  the model for Dannan's transport tests and the port gate for step 3.
- (2026-11-03 evening, this plan) **One version number for the
  repository**, both libraries: `project(dannan VERSION x.y.z)`, the tag
  and `--version` agree. The first tag is cut when Macha first links
  Dannan (step 3); Macha and later AthenaSIP pin a tag, and
  `kBuildIdentity` there includes it.
- (2026-11-03 evening, this plan) **Dannan's transport exposes what its
  tests need.** Two behaviours could not be proven in Macha for want of a
  seam (best-effort broadcast skipping a busy route table; session
  reaping). Dannan is new code: a session count in the server's work
  stats (kept out of the Status JSON) and a test-only hook on the client's
  route table are part of the design, not a retrofit.
- (2026-11-03 night, the maintainer) **Macha is the first customer. The design is not
  complete until Macha can be refactored to use it.** Every call Macha
  makes into its network layer must map to a `dannan::Context` operation
  before step 5 counts as done; the audit is
  `TODO/2026-11-03-macha-on-context-audit.md`, and its gaps are work, not
  questions. Macha's move to the new wire is a flag day (no protocol 23
  mode), noted for the Macha agent.
- (2026-11-03 night, the maintainer) **Dannan is a new project with its own wire,
  versioned from 1.** It is not an extension of Macha; Macha is refactored
  to use it. This supersedes "the one rule"'s byte-for-byte protocol 23 and
  "No byte moves": nothing of protocol 23 binds Dannan (versions, labels,
  magics, record layouts, message numbers). Dannan does everything to
  maintain the PHY (reachability included). Level 0's wire is
  `TODO/2026-11-03-dannan-wire-v1.md`. Docker is used for the compose test
  bed and nothing else. Identity (step 10) waits.
- (2026-11-03 night, the maintainer: "Continue please" on the draft) **Wire v1 as
  drafted, with the draft's recommendations.** The Macha interop suite
  (`dannan_interop_tests`), `tools/macha_types.h` and `dannan-node`'s Macha
  script go with protocol 23; "a test is never deleted" is set aside for
  them by this decision, and a Dannan-only two-process test replaces the
  cross-process proof. A punched or relayed connection proves its token
  inside the hello (a purpose byte and a 16-byte token), so a stray
  connection fails the handshake.
- (2026-11-03 night, the maintainer) **Dagda is its own project, with its own agent.**
  Steps 7 and 8 leave this plan; the `dagda/` stub here goes when that
  project starts. **Macha is not moving yet:** the port steps (3, 6) are
  the main Macha agent's, later. This plan finishes Dannan's own code:
  steps 4 and 5, then Dannan's part of 9 and 10. Dannan's API is not
  stable and nothing uses it yet: change it freely.
- (2026-11-03 night, the maintainer) **Core message types, and user types under the
  user's control.** Dannan's own types (`with_dannan_types`) are core:
  their rules are Dannan's and cannot be redefined. Every other number is
  a user type: its lanes, classes, notification flag and handling are the
  registrant's. A user type nobody on this node registered is not
  policed (either lane, any class) and goes to the user's handler, a
  notification included, so a peer is never cut off for traffic this node
  does not host. Supersedes protocol 23's default (control lane, control
  frame only) for unknown numbers; no byte changes. Follows from it, the maintainer's
  question the same night: a handler reports whether it processed the
  message, failed, or does not recognise it.
- (2026-11-04, the maintainer, asked in turn) **The stack's open questions, settled:**
  - Tag `0.2.0` (wire v1, level 0, channels, the roster feed, the
    foundations) with CMake install and export targets, so Dagda, Lugh
    and Lir use `find_package(dannan)` and pin a tag.
  - Message numbers: Dannan's core types are 1 to 511; user types start at
    512 and are registered freely, with no blocks per system; a clash
    shows at registration.
  - Scope: Dannan declares `contract::ScopeSource` and `Identity::verify`;
    Lugh installs the source; Dagda's write-floor replicas and every other
    hosted system check through it.
  - Lugh's data is two domains: `lugh/scope` (every node a member) and
    `lugh/secrets` (Lugh nodes members, others blind at most).
  - One process per node: a product binary links Dannan (and Dagda) and is
    a node; several may share a machine; each advertises what it hosts.
  - Node revocation in step 10: a CA-signed, grow-only revocation record,
    gossiped and persisted, refused at every handshake, sessions closed.
  - Authentication in Lir is pluggable, as everywhere else; a Lugh plugin
    depends on Lugh. Token verification does not move into Dannan.
  - HTTP stays each hosted system's (the plan's rule); no shared HTTPS
    stack.
- (2026-11-04, the maintainer, asked in turn) **Identity (step 10)**: certificates
  are X.509 through OpenSSL; they are wire version 2 beside version 1 (a
  node with both speaks both, so a cluster moves node by node); the CA key
  is offline, used through a `dannan-ca` tool, never on a node. The design
  is `TODO/2026-11-04-identity.md`.
- (2026-11-03 night, the maintainer) **One stack, no duplicated code.** Dagda and
  Lugh use Dannan as their network layer; Lugh uses Dagda as its
  datastore. No code is duplicated between the three projects: what more
  than one needs (crypto primitives, durable files, the thread-safety
  contract, time sources, logging, the Status surface) lives once, in the
  lowest project that needs it, and the others link it.
- (2026-11-03 night, the maintainer) **Identical styles, conventions and layouts** in
  all three: one `.clang-format`, one header block form, one naming scheme,
  one repository layout (`<project>/<subsystem>/`, `<project>/include/`,
  `<project>/test/`, `tools/`, `docs/`, `TODO/`), one branch and tag
  scheme, one set of rules in `CLAUDE.md` and `CONTRIBUTING.md`.
- (2026-11-03 night, the maintainer, in Lugh's session, as Lugh reported it) **Lugh
  goes callback style**, Dannan's shape (deadline, caller's executor,
  completion posted there): no completion-token wrappers are needed by
  anyone now. **Lugh links Dannan now**, by FetchContent of this tree pinned
  to `feature/wire-v1` at `785e393`, moving to a tag once one carries the
  Context. Lugh adopts Dannan's conventions; its header is "Lugh - The
  Directory".
- (2026-11-03 night) **The stack has one document**:
  `../dagda/TODO/2026-11-03-stack.md`, kept by the Dagda session, checked
  by the Dannan and Lugh sessions. What each of Dannan, Dagda and Lugh owns,
  the seams (the scope source Dannan declares and Lugh installs, Lugh's two
  domains, one process per node, node revocation) and what is proposed for
  the maintainer are there, not restated here.
- (2026-11-03 evening, this plan) **0.91.3's reply rule is a transport
  behaviour Dannan keeps.** A reply to an admitted request is never
  dropped for capacity: refused by the outbound limits or the memory
  lease, it is replaced by a 503 `transport_busy` error reply allowed past
  the cap, up to `max_pending_rpc_requests` extra; past that the
  connection closes, which answers every pending call. In the inventory's
  transport table.

- (2026-11-03 evening, this plan; step 2's open design points) **How a
  request is executed is a property of its type's registration**, beside
  its lanes and classes: `fast_control` (answered by the small pool that
  never queues behind storage-backed control work; Dannan's `ping` and
  `members`) and `ordered` (one job per peer at a time, in arrival order,
  across a small bounded pool; Macha registers its three metadata
  mutations so). The server's per-class queue byte bounds and worker
  counts are Dannan's, as `ServerLimits`; the ordered class's job and
  byte bounds are the registering hosted system's to size, defaulting to
  Macha's.
- (2026-11-15, the maintainer) **Agreement has no root except at genesis, and a
  genesis root may be a single node.** Names hash to a fixed slot table.
  Every slot is touched at founding on the genesis set, and from then on
  each slot's set is recorded in its own acceptors and moved only by
  `transfer()`. No node or operator holds a role after genesis
  (`TODO/2026-11-15-agreement-design.md`, "Reconfiguration").

## Standing rules

### Style

Dannan's own, from the first file; `CLAUDE.md` and `CONTRIBUTING.md`
state it and `.clang-format` enforces the mechanics. C++20; Google base
at 160 columns, two-space indent, braces on the same line, `clang-format
-i` on the files you changed; headers `.h`, `#pragma once`, the header
block, includes relative to the repository root (`dannan/net/channel.h`);
`snake_case` for functions, methods, variables and files, `PascalCase`
for types, `_name` for private and protected members; namespaces
`dannan::<subsystem>` (`net`, `membership`, `placement`, `sched`,
`supervise`, `identity`, `reach`, `status`) and `dagda::<subsystem>`
(`domain`, `store`, `ledger`, `heads`, `lease`); no emoji anywhere;
comments sparse and about intent; collaborators by constructor reference
or `std::shared_ptr`, no frameworks, no DI containers, the io context the
only process global; each component logs through its own scoped logger
with a short scope name; prefer writing a thing by hand over adding a
library and say so when a dependency really is right (Boost.Asio,
OpenSSL and GoogleTest are the dependencies); version and document every
public, wire and on-disk format; time is injected and no test sleeps past
an interval; tests from the design, the laws and the inventory, never
from current behaviour; `main` the last stable state, `develop` where
work happens, `feature/<name>` off `develop`; tags exactly `x.y.z`,
annotated, no `v`, cut from `main`; commit subjects short imperative
prose, unrelated changes in separate commits, no ticket prefixes, no
Claude or Anthropic attribution anywhere; GPLv3.

Kept from Macha, because they are substance: thread-safety annotations
(`dannan/contract/thread_safety.h`, `DANNAN_GUARDED_BY` and the rest,
checked by clang `-Wthread-safety`) on every guarded member with the lock
order stated where two locks nest; contracts that declare their waits and
thread safety beside each operation (`static constexpr Waits x_waits`,
`ThreadSafety x_safety`) with the wait guard as the runtime check; the
executor rule (a completion is posted to the executor the caller passed,
never called inline, even when the answer is already known); the
contract versioned and async from its first version, a hosted system
reporting the version it was built against and a mismatch refused;
registries keyed by name; a read that finds nothing succeeds with an
empty value and failure means the operation could not happen; every call
returning evidence for anything that happened on more than one node; no
call waiting without a deadline. The habits of Macha's runner (`TestCluster`
and `TestGate` over sleeps, `SteppedTime` over the clock, case names that
state the invariant) as GoogleTest fixtures under `test/`.

Macha's style is Macha's: a port step in `../macha` matches the file it
lands in (`.hpp`, the SPDX line, `name_`, LLVM base at 100 columns) and
changes nothing it does not have to.

### Workflow

- **Two kinds of session.** A *build session* takes one step here, on
  `feature/<step>` off this repository's `develop`, writes the tests from
  the inventory rows and the design first, watches them fail, builds the
  piece, leaves `dannan_tests` and `dagda_tests` green with the counts in
  `COMPLETED.md`. A *port session* takes one port step in `../macha`, on
  `feature/dannan-port-<piece>` off Macha's `develop`, pins this
  repository's tag, swaps the piece, leaves the port gate green with its
  counts in `COMPLETED.md` here and in the "Dannan extraction" section of
  `../macha/TODO/ACTIVE.md`, and deploys to the live nodes when convenient.
- **Report findings before fixing; prove a mechanism before editing.**
  Design questions go to the maintainer with a recommendation. Decisions above are
  not reopened without asking.
- **A port is reviewed as a swap**: the Macha diff removes a file or a
  class and adds a registration or an adapter; a content change beyond
  that is named in its commit, and a port whose diff touches a codec is
  wrong.
- **Every step ends with the inventory updated**: each row it covered
  gains the Dannan or Dagda case that proves it here; each row a port
  touched keeps its Macha case as the port's gate.
- **Nothing is published.** The design is in this folder, and these
  copies are the masters (the maintainer, 2026-11-04). Pushing is a separate,
  explicit request.
- Code lookups in Macha and AthenaSIP go through basemind when it is up
  (all three trees carry `basemind.toml`); rescan after edits.
- The Macha session (another Claude session on this machine, "Macha
  Server") works in `../macha` on the maintainer's Macha asks; tell it before a port
  session touches that tree, and it tells this one.

---

## Parked

Kept in the design or the tree, not on any list:

- **HTTP route registration** (`HttpServer::register_route`, the prefix
  list at `macha/src/service/service.cpp:73`): Macha-internal; wanted only
  when two hosted systems share one listener.
- **A stable C ABI for hosted systems**: not promised; a hosted system is
  rebuilt with Dannan.
- **A UDP lane and UDP punching**: after level 0, when the relay-only
  count on real clusters says it is worth it.
- **The relay as an anonymity property**: it is not one and will not be.
- **A many-tenant public service**: a different trust and placement model.
- **The blind domain role in practice**: specified at step 7, exercised
  when a cloud node is first asked to hold what it cannot read.
