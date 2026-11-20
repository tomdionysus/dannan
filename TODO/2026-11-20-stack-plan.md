# Dannan for the Cluster Stack: the plan after 0.9.2

Started 2026-11-20. This is Dannan's own plan for what the Cluster Stack (Macha, Dannan, Dagda,
Lir, Lugh, Louhi, Mimir, Kvasir; not AthenaSIP or Miach) needs of it. The placements were settled
among the sessions and are kept by Lir Development Cluster in
`../lir-develop-cluster/TODO/2026-11-20-cluster-stack-placement.md` ("The converged plan" ->
"Dannan"). This file turns them into a baseline Dannan can build from: who needs each item, the
shape, the test that proves it, and what is still open.

**State.** Planning only. the maintainer, 2026-11-20: "You should start planning now." Code stays held
(Q7: "Keep holding"). A requirements request went to all seven consumers the same day; their
answers fill the "Needs" and "Proof" lines below. Nothing here is built.

## the maintainer's rulings this plan rests on (2026-11-20)

- The audits: no duplicated functionality; no special role or node; failures are never the
  user's problem except in exceptional circumstances; no special-case code for a single node
  ("you are all leaderless, multimaster soft cluster systems").
- Placement (Q6): anything two or more projects need lives in the lowest project they all reach;
  if one already depends on the other, the lower one keeps it.
- Q1: HTTP and JSON move into Dannan; Boost.JSON becomes a Dannan dependency.
- Q2: register maps order by (hlc stamp, node id), safe only with the stamp bounded (item 6).
- Q3: a hand-written authoritative DNS responder on every public node, in Dannan. This changes
  the design text "Dannan has no namespace and no resolver; bind9 serves"
  (`2026-11-02-dannan-plan.md`, "DNS"); the revision is part of item 24.
- Q4: an absent node never leaves the majority count without an operator; retire is one
  cluster-wide command from any node.
- Workflow: features on branches, one squash commit onto develop; all builds and tests on the
  Pi cluster; sanitizers at a feature's end before merge, or to hunt a bug; test once; no flaky
  test tolerated; no cluster core held idle; public repositories keep CI green and carry the
  relevant badges.

## Order

Three minors, so dependants can pin early. 19 only if Macha asks.

- **First minor:** 13, 1, 17, 6; small between them: 15, 9, 12, 16, 18 (store tests), 20.
- **Second minor:** 2, 7, 8, 10, 11, 14, 23, 24 (HTTP and JSON).
- **Third minor:** 3, 4, 5, 22, 25 (DNS), the rest of 18. These depend on 2 and 7 and get the
  simulator first.
- After: the earlier 0.10.0 list (DTLS and UDP for listen, the peer-witnessed high-water mark,
  loader promotion, Mimir's cross-slot commit), unless a waiting project says one blocks it.
- 0.9.3 (the hybrid clock's ceiling written only for stamps issued) stays planned for after
  Macha's cutover; it is folded into item 6 if the first minor comes first.

Standing work outside the items, also held: the two Dependabot pull requests (actions/checkout
4 to 7, codecov-action 5 to 7), the two open flakes (`LevelZero.AChunkLostWithTheSendersSession...`,
`ChildProcess.ChildrenStartOnManyThreadsAtOnce` under TSan), and README badges (C++20, platforms).

## Items

Each: who needs it, the recorded shape, what is open. "Needs" and "Proof" are filled from the
consumers' answers.

### 1. In-memory AcceptorStore (first minor)
- Users: Mimir, Kvasir, Louhi. Dagda keeps the durable one.
- Shape: beside `agree::AcceptorStore`; meets the header's contract text: keep() all or nothing,
  done never inline, need_whole for a delta it cannot apply, value() by exact ballot. Not ready
  after a restart until catch_up has run (an in-memory store that voted after losing state could
  commit two values for one name).
- First test: a restarted store answers ready() false and is refused as an acceptor until
  catch_up completes.
- Open: whether it lives in libdannan or in dannan::testing as well.

### 2. The register layer over agreement (second minor)
- Users: Mimir (routes 600/601, operation ids, version_of), Kvasir, Louhi.
- Shape: a catch_up message asking a peer for a slot's names and states, answered from
  `load(slot, {}, every=true)`, so no store gains a method; operation ids and an applied window
  so a retried change applies once (not a delivery guarantee); ballots as ordered bytes; a
  "something moved" notice after a commit to the replica set, with an opaque version and an
  optional bounded inline payload, at most once, plus gap reporting or per-slot anti-entropy.
- Open: the notice's bound and whether gaps are reported or repaired.

### 3. Replica sets follow membership (third minor)
- Users: Mimir (Mi-1), Louhi (S3), Kvasir.
- Shape: Dannan drives transfer() itself; each slot's set is rendezvous placement over the
  agreed roster; any node may start a transfer, and concurrent starters converge because
  transfer() is agreed. transfer() needs a majority of the old set; a set that has lost its
  majority for good stays an operator act.
- Until it ships, operators must not retire a genesis member while slots are still on genesis.

### 4. Founding without a founder (third minor)
- Users: every project; Lugh's double-founding worry; Dagda's lowest-id formation goes.
- Shape: one explicit, once-only act any node can carry out, writing the genesis record. A
  double founding is refused loudly (the table fingerprint already refuses another cluster's
  requests) with a Status line naming the other cluster. Join by a name (item 25).
- Open: what the genesis record holds, and how a node learns it on join.

### 5. Retire as one cluster act (third minor)
- Users: Louhi, Macha, Dagda, Lugh.
- Shape: committed through agreement from any node, applied by every node from that record;
  the operator marks the node draining first (item 7), placement avoids it, Dagda evacuates
  and confirms, then retire. Retire removes the node from the agreed roster, membership's known
  set and placement at once, not after 30 days.

### 6. Clock rules (first minor)
- Users: Dagda (D6, tombstone maturity), Lir, Louhi, Lugh, Macha, Mimir (Mi-2).
- Shape: an NTP-style offset per peer from the heartbeat exchange. A node leaves ready when its
  offset exceeds the bound against more than half of the agreed roster, itself counted at zero
  (no special case by cluster size), and such a node stops issuing stamps. Acceptors refuse a
  request whose issue stamp `receive()` would not merge; `HlcClock::admit(stamp, peer)` is the
  one check for every other receiver, and merged=false means refuse, never apply. max_ahead's
  default drops from one minute to a few seconds tied to the offset bound (a minor, in the
  CHANGELOG). Membership stamps every record change max(wall, last+1).
- Open: the bound's default.

### 7. Readiness and health (second minor)
- Users: Lugh, Louhi, Macha, Kvasir, Dagda (store health), the HTTP health route.
- Shape: status::Readiness with registered checks; built-ins: memory reserve exhausted, class
  gates saturated, clock offset, certificate expiry, store not ready. A health advert carrying
  the state only (ready, degraded, draining, not ready), changed on a transition, never figures.
  placement() skips nodes not ready by default.

### 8. Slow peers (second minor)
- Users: Lir (the SUBACK hold), Dagda (the joiner's survey), everyone through gossip.
- Shape: a per-peer answer bound on advertise-with-deadline and bulk publish (the larger of a
  floor and a multiple of that peer's measured latency; a peer past it is counted in `missed`
  as slow, never waited for). The heartbeat exchange, revocation and identity reset run in
  parallel and are gossiped; dial-back rotates through peers.

### 9. Encodings and Prefix (first minor)
- Users: Lugh, Louhi, Macha, Mimir.
- Shape: encoding/ gains base64, base64url (padded and not), base32; hex moves there from
  identity/crypto.h with a forwarding alias for one minor. listen::Prefix refuses host bits at
  config load with a clear error (a behaviour change, in the CHANGELOG) and unmaps IPv4-mapped
  addresses.

### 10. Keyed limits (second minor)
- Users: Lugh, Macha, Mimir, Kvasir.
- Shape: a keyed bucket map with a Retry-After wait; a hard bound on it and on Backoff, evicting
  the key least recently touched; the key an opaque caller string. The contract recommends
  keying by name and address together plus the listener's global bucket, since flooding
  made-up keys can evict a real name's penalty.

### 11. The credential (second minor)
- Users: Lugh, Kvasir.
- Shape: Argon2id, 16-byte salt, 32-byte hash; cost bounds refused before any work; make,
  verify, needs_rehash; constant work with a decoy for an unknown name; the PHC string
  (unpadded base64) as the text form, binary fields of the same struct allowed in a record.

### 12. SHA-1 and HMAC-SHA-1 (first minor)
- Users: Mimir (script ids), Lugh (TOTP); Macha only if it computes SHA-1 itself.

### 13. Contract vocabulary (first minor)
- Users: Lugh, Louhi, Dagda, Kvasir, Mimir.
- Shape: contract/ gains Completion (posted to the caller's executor, never inline, exactly
  once, no later than its deadline), Outcome, Fault, Deadline, Done; the ack-level enum
  ephemeral, persisted, decided, replicated.

### 14. Node-program helper (second minor)
- Users: Mimir, Kvasir, Lugh, Louhi, Dannan's own dannan-node.
- Shape: io threads, HOST:PORT, standard cluster, node, timing and log settings, TLS from
  certificate paths, signals, drain, --version, --retire, line output.

### 15. RetryPolicy::unbounded (first minor)
- Users: Louhi, Lugh, Dagda. Interim: max_failures_in_window at the size_t maximum, failure_window
  1 ms, max_failing_duration 0 (Dagda 97a079e).

### 16. A random 16-byte id type in identity (first minor)

### 17. dannan::testing (first minor)
- Users: Lir, Dagda (D13), Mimir, Kvasir, Lugh, Louhi.
- Shape: one static library, headers under include/dannan/testing/: the sim World, SimNodes,
  SimNetwork and SimDisk, stepped and manual time, the transport seam and a fake Stream,
  in-process cluster fixtures, RecordingSink (with Lugh's scope and text()), the fuzz
  Registration and driver. Built behind DANNAN_BUILD_TESTING_EXPORT for FetchContent; packaged
  as libdannan-testing-dev. GoogleTest a dependency of this target only. The sim API is a
  contract, versioned and kept within a minor.

### 18. Missing tests
- A keep() that fails; ready() false in a real node; the founder stopped after growth;
  per-operation progress under faults in the sim; the clock and readiness rules giving the same
  results on one, two and five nodes.

### 19. durable_create_directories, only if Macha calls it on its own.

### 20. Certificate expiry on Status; the external-IP lookup URL configurable, off by default.

### 22. A resizable agreement slot count (third minor)
- Users: Mimir (the maintainer's ruling 41: capped at 8192, resizable on a running cluster), Kvasir
  (ranges grow without limit; a full slot cannot be transferred).
- Fact at 0.9.2: every agreement request carries the slot table's fingerprint, and an acceptor
  with another table refuses it, so a count change is a table change every node must agree on.
- Open: the shape. To be designed once the answers on sizes, frequency and availability are in.

### 23. A process runner (second minor)
- Users: Louhi (ip, nsenter, sysctl, nft), Macha (mobitool).
- Shape: from Louhi's `louhi/net/command.h` (feature/task-namespace, dbdc68b): fixed argv, no
  shell, the program resolved on a fixed system PATH, env PATH and LC_ALL=C only; stdin and
  output on Unix socket pairs (no SIGPIPE); output bounded with truncation said, or to a file
  that can be tailed; the process group killed at the deadline; a nice value; cancellation on
  stop; reaped by polling, never a blocking wait; one posted completion.
  test/fixtures/child_process.h moves onto it.

### 24. HTTP and JSON (second minor; the maintainer said yes)
- Users: Lugh, Louhi, Lir, Macha.
- Shape: a bounded HTTP/1.1 server core, router, query and form parsing, an HTTP and HTTPS
  client (Louhi needs plain http://), and a health route behind status::Readiness, in listen/,
  starting from Lugh's. JSON through Boost.JSON, its compiled unit carried once, in Dannan.
- Open: Lugh's starting files and bounds; whether Unix-socket streams come too (Louhi's
  local_http). The design text's "HTTP is the hosted system's" line is revised with this item.

### 25. DNS answered by every node (third minor; the maintainer said yes)
- Users: Lir, Kvasir, Lugh, Mimir (a client address that survives a node), every project
  (joining by name, item 4).
- Shape: a hand-written authoritative responder on each public node for one service zone,
  answering A, AAAA and SRV from its own roster (active nodes advertising the service's
  capability), with a short TTL; the operator delegates the zone once to several nodes. Nodes
  behind NAT are not returned. No store and no zone primary.
- Open: the record types and TTL each consumer needs; the design text's DNS section is
  rewritten with this item.

## Requirements request (sent 2026-11-20)

To Macha Server, Dagda, Lugh, Louhi, Mimir, Kvasir and Lir: A, every Dannan call used today
(the set not to break); B, for each item, the calls they would write against, the step it
blocks and their stand-in, the test that proves it, and what in the shape would not work; C,
their threads, deadlines and sizes. Answers are reconciled here, and each consumer confirms the
shapes before any code.

| Consumer | Answered | Notes |
|---|---|---|
| Macha | 2026-11-20 | below; holder shape replaced by the maintainer's ruling (inject a logger and the Context) |
| Dagda | 2026-11-20 | below |
| Lugh | 2026-11-20 | below; confirmed the fault enum and keyed map |
| Louhi | 2026-11-20 | below; confirmed the fault enum and item 3 |
| Mimir | 2026-11-20 | below; raised names per slot and atomic commit |
| Kvasir | 2026-11-20 | below; asked for atomic commit by the third minor |
| Lir | 2026-11-20 | below; raised selective interest at scale |

## Items added by the answers

- **26. Atomic commit across replica sets** (Mimir, Kvasir; third minor, after 3). From Mimir's
  proposal (`../mimir/TODO/2026-11-15-cross-slot-commit-proposal.md`) with Kvasir's corrections.
- **27. Config extensions** (Macha; all config users gain): lists of maps, nested sections, size
  units, octal modes, doubles, tristate auto, bracketed IPv6 endpoints, *_ms durations, unknown
  keys refusable as an option, a cross-field validate hook with warnings. Second minor.
- **28. Logging for injection** (Macha, with the maintainer's ruling): a logger constructible before any
  node from a root-owned sink; LogSink::enabled lock-free from the first call; a race-free sink
  swap with shared_ptr handoff; an emit bypassing the filter, safe from foreign threads; capture
  sinks in dannan::testing; Context::logger(name). First minor (small, and every port step
  logs).
- **29. Observations for injection** (Macha): a root-owned Observations handed to the node (one
  writer), elapsed_us, a histogram handle recording without a per-record lookup; Context
  exposes it with threads(), readiness() (const) and the wait guard (record and throw modes).
  Second minor.
- **30. RPC parity** (Macha): a thrown handler answered with a 500 reply; unbind waits for calls
  in flight and an unbound type answers 501 (checked); the stall journal line and
  transient-failure classification checked against Macha's. Second minor.
- **31. Selective channel interest** (Lir; Mimir's prefix pub/sub benefits): interest off the
  node record onto its own CONTROL exchange, a per-channel summary with deltas, versioned per
  node. Third minor unless Lir needs it sooner.
- 19 becomes firm (Macha): public durable_create_directories and durable_sync_directory and a
  directory-mode parameter, default 0700. First minor (small).
- Placement parity (Macha): Macha's placement vectors as Dannan tests, any byte difference
  Dannan's to fix. First minor, before Macha switches.

## Open questions this round leaves

- **Paged transfer and catch-up** (Mimir 10^5 to 10^6 names a slot, Kvasir up to thousands):
  transfer() and catch_up over any number of names, value-less, in pieces, against max_names
  4096 at 0.9.2. The first design question of the third minor; it shapes 3, 22 and 26.
- **Ack-level enum and Dagda's "durable"**: the stack's four, Dagda mapping its fifth value onto
  them, unless the maintainer rules otherwise. To confirm with Dagda.
- **Order of the second and third minors**, now heavier (24, 27, 29, 30 in the second; 26, 31 in
  the third). To be weighed once the shapes are drawn; the first minor's order is unchanged
  apart from 28, 19 and the placement vectors joining it.

## Answers

### Dagda (2026-11-20, pinned 0.9.2 in Dagda 0.3.2)

Used today, the set not to break (library includes, count): contract/thread_safety.h 29,
net/codec.h 21, identity/crypto.h 24, contract/time_source.h 20, identity/node_id.h 19,
status/log.h 19, sched/work.h 16, identity/durable_file.h 14, context.h 13 (nodes()
active/all/advertise/snapshot/directly_reachable, routes() call/call_and_wait, events()
kind/notify/count, observations(), status(), time(), identity(), logger), membership/node_record.h
6, and fewer of identity/cluster_key.h, key_custody.h (derive_local), sched/walk.h, observation.h,
placement.h (by_capacity, logical_capacity), contract/scope.h, node_identity.h, published.h,
secure_memory.h, net/message.h, routes.h, supervise/startup.h, supervised.h, version.h,
certificate.h, signing.h, outbound.h, scheduler.h, session.h (ServerLimits), node.h,
class_waits.h, duty_share.h; pending merge: sched/cpu_pacing.h, sched/retry.h. Tests: test/fixtures
temp_dir.h, stepped_time.h, gate.h, child_process.h, ports.h, identity/argon2.h,
membership/agreed_roster.h. Heaviest ABI exposure: codec, crypto, time_source, work, walk,
Context's nodes/routes/events, NodeRecord's adverts.

Needs, by item:
- 1: not used; Dagda writes the durable store against agreement.h at 0.9.2 and asks that the
  interface's ABI hold across the first minor, or be told what moves.
- 4: "this cluster is founded" as a query and a notice, plus the founding identity. Blocks the
  removal of Dagda's lowest-id formation and the joiner's single-node branch
  (dagda exchange.cpp:936-948, :1328-1335), which break the maintainer's no-single-node rule today.
  Proof: a one-node and a three-node cluster form a space through the same path, and a second
  founding is refused.
- 5: "draining" readable on the roster by every node before retire commits; a notice when a node
  is retired; retire refuses until Dagda's "evacuated" evidence holds, through a registered check.
  Proof: a draining node's data is all elsewhere before retire, and retire refuses while it is not.
- 6: HlcClock now, receive, admit(stamp, peer); the offset reading. Blocks D6's HLC form (stand-in
  the Lamport seam, never released) and D-2 (stand-in the wall clock). Issuing a stamp while out
  of bounds fails at once, never waits. Proof: a stamp from a node far ahead is refused, never
  applied, in a register-map fold.
- 7: register a check returning {ready, why}: pool or control store full, backend offline,
  durability failure, evacuation pending. Proof: a full control store takes the node out of ready
  and placement skips it. Not blocking.
- 13: Dagda's store::Durability is ephemeral, persisted, replicated, durable; Dagda keeps
  "durable" beside the stack's four (the maintainer, 2026-11-15) and asks that the enum allow it.
- 15: replaces the four fields in Dagda's two backoffs. Not blocking.
- 17: keep or forward temp_dir, stepped_time, gate, child_process and ports by name when they
  move into dannan::testing (dagda::test_fixtures builds on them); SimDisk for power-cut cases.
- 18: the failing-keep case; Dagda mirrors it.
- Not used: 2, 3, 8, 9, 10, 11, 12, 14, 16, 19, 20, 22, 23.

Constraints: called from Dagda's worker threads, io threads in handlers, and callers' threads in
blocking forms; never blocks an io thread (call_and_wait only off it). Completions posted, never
inline. Every synchronous call takes a deadline or a no-progress bound. Adverts: one slow key,
dagda.spaces, plus the store's few; never commit-driven. Messages up to 16 MiB plus tree leaves
near 8 MiB; tested to 5 nodes, planning for dozens; 1,280 spaces and logs per node in Lir's plan.
advertise, nodes().active() and event notify must stay non-blocking.

Reconciled into the items:
- 5 gains a registered retire check: retire refuses while any check says no, with its reason.
- 13: the ack-level enum is the stack's four in order; Dagda's "durable" stays Dagda's own value
  mapped onto it, not a fifth member of Dannan's enum, unless the maintainer rules otherwise. Open with Dagda.
- 17: the moved fixtures keep their names under dannan::testing, with the old headers forwarding
  for one minor.
- 1 keeps agreement.h's ABI through the first minor: the in-memory store is added beside it,
  nothing in the interface moves.

### Mimir (2026-11-20, pinned 0.9.2)

Used today, not to break:
- agree/: the whole AcceptorStore interface (mimir/cluster/store.h:36); Slot, Name, Bytes, Ballot,
  AcceptorState, SlotState, Held, max_slots; Context::agreement() serve/unserve, found, change (with
  Changed{values, result, repeatable}), read, catch_up, table().slots and table().genesis; Outcome
  committed, unknown, values, result, ballot, why (cluster/agreements.cpp:161, 216, 255; space.cpp).
- context.h: nodes advertise, self, active, NodeRecord adverts; routes add(Registration) before
  start, bind/unbind with Handled::processed or failed, call with CallOutcome, net::refusal_of
  (types 600, 601); channels open(notify), subscribe (plain and pushing with Pushed), unsubscribe,
  publish, ChannelMessage; time executor() and after(); identity id() and durability_epoch().
- node.h: NodeConfig state_path, key, listen_host, port, advertise_host, bootstrap, node_name,
  failure_domain, version, log, agreement_slots, agreement_genesis; Node start, stop, port,
  context (tools/mimir_node.cpp:206-252).
- identity: load_cluster_key, ClusterKey::from_master, NodeId parse/to_string,
  load_or_create_node_id, sha256, hex, random_bytes, constant_time_equal.
- net/codec.h Writer and Reader; listen/ StreamListener, ListenerConfig, Certificates, Stream,
  connector (MIGRATE), server; config/ Reader, Setting, Settings, ConfigError; status/log.h
  AsyncSink, console_sink, ScopedLogger, parse_log_level/format; thread_safety; WallClock.
- Test fixtures: stepped_time.h (SteppedWall), temp_dir.h.

Needs, by item:
- 1: constructed per space and passed to serve(); not ready after restart until catch_up, then
  set_ready(); posted completions; keep() all or nothing; need_whole; value() by exact ballot;
  state(slot, name) and size() for tests and Status. Proof: ClusterStore's tests, ClusterSpace
  and ClusterDatabase (three real nodes, restart and catch-up) pass with Mimir's store deleted.
- 2a: Dannan owns recovery: recover(space, done) pages names from a ready peer, runs catch_up
  per slot, moves to another peer after a second on failure, set_ready()s the store at the end,
  and hands the hosted system each register (slot, name, value, accepted ballot) to fill its view.
  Stand-in agreements.cpp:113-186.
- 2b: commit notices to the register's other replicas, best effort: name, version, optional
  inline value up to a bound (Mimir 64 KiB), delivered as on_committed(space, slot, name,
  version, optional value); versions compare as ballots. A notify channel is acceptable.
- 2c: operation_id() unique across the cluster and restarts (node, epoch, u64 counter; 40
  bytes); helpers keeping applied ids with opaque replies inside a value, bounded by count
  (1024) and a horizon (120 s against a later issue time), refusing rather than evicting a live
  id (Mimir's note_applied).
- 2d: version_of(ballot) as order-preserving bytes.
  Proof for 2: ClusterDatabase.ANodeRestartedWithoutItsStateCatchesUpBeforeItVotes,
  APublishAndANotificationReachEveryNodesSubscribers and the CommandAgreement notice tests pass
  with routes 600 and 601 deleted.
- 3: change() and read() without naming the set: Dannan resolves the slot's current set and
  version (preferred), or table().replicas(slot) returns them; "is this node a replica of slot
  S" (forwarding, ruling 38); a watch on slot moves. Blocks stage 3 step 4 and forwarding.
  Proof: three nodes gain a fourth and an operator retires one; every key stays writable and
  1000 keys written before are read from every node.
- 6: acceptors refuse a change whose issue time is beyond theirs by more than the bound, saying
  why; Mimir takes issue time from hlc now(). Proof: a node 60 s fast cannot expire another
  node's key early, and its writes are refused with a reason.
- 10: KeyedBuckets(capacity, rate, max_keys), take(key) -> optional delay, LRU-bounded,
  thread-safe, keyed per client address. Proof: ten failed AUTHs from one address are delayed,
  another address is not.
- 12: sha1(span) -> array<20> and hex. Proof: Mimir's sha1_hex removed,
  CommandScripting.Sha1IsTheBodysDigest passes.
- 14 and 17: whatever shape; RESP and RESTORE fuzz targets link the harness.
- 22: practical count 1024 (cap 8192); an operator act, once or twice in a cluster's life;
  online under write load; a slot at a time briefly unwritable (seconds), reads may serve the
  old slot meanwhile; Dannan's part an agreed count change with a map from each old slot to its
  new ones and per-slot handover with transfer's guarantees; Mimir maps 16384 hash slots onto
  shards in contiguous ranges, so a doubling splits each shard in two.
- Not on the list, Mimir's largest block: multi-key atomic commit (the 0.10.0 cross-slot
  commit); every multi-key write is refused today (ruling 35).

Constraints: agreement callbacks run on Dannan's executor, and Mimir's Change takes its database
mutex inside, so Dannan never calls a Change while holding a lock Mimir's code could wait on.
Two io threads, never blocked. Deadlines: change and read 5 s each, retried to 15 s while
unknown; catch_up 10 s; notices 2 s. Names per slot 10^5 to 10^6; values to 512 MB, notices
inline only at 64 KiB or less; 3 to about 32 nodes; thousands to tens of thousands of
single-key changes a second per node. publish, advertise and notice sends never block.

Reconciled, and open:
- **Conflict: names per slot.** Mimir plans 10^5 to 10^6 names a slot; transfer() refuses a slot
  above max_names (4096) at 0.9.2. Items 2a (recovery) and 3 (replica sets) both move whole slots,
  so both must page: a transfer and a catch-up over any number of names, value-less, in pieces.
  This is also Kvasir's alternative to 22, so it serves both. It goes into item 3's shape and
  is the first design question of the third minor.
- **Order: multi-key atomic commit.** It is Mimir's largest block and sits after the third minor.
  To be weighed against the other answers before the order is fixed; Mimir and Kvasir designed
  it together (`../mimir/TODO/2026-11-15-cross-slot-commit-proposal.md`).
- 1 gains set_ready() after catch_up, and state(slot, name) and size() for tests and Status.
- 2a is Dannan's whole recovery loop, not only the names exchange.
- 3 resolves the set inside change() and read() when none is named, and adds is_replica(slot)
  and a slot-move watch.
- 22's shape starts from Mimir's numbers: an agreed count change, each old slot mapped to its new
  ones, per-slot handover under transfer's guarantees, online.

### Kvasir (2026-11-20, pinned 0.9.2)

Ground rule from Kvasir: a cluster that may be one node, with nothing special-casing one node.

Used today, not to break:
- contract/thread_safety.h Mutex, Lock, annotations (catalog, txn/store, server/sign_in).
- contract/time_source.h TimeSource, TimePoint, steady_time_source (pool_checker, statistics,
  sign_in, commands, tools/kvasir_node.cpp).
- identity/crypto.h sha256, constant_time_equal, cleanse, random_bytes, hex, unhex, Hash;
  identity/secure_memory.h Secret; identity/argon2.h argon2id and its params (accounts).
- net/codec.h net::Bytes.
- config/config.h Reader (config_variable, default_path, settings, environment, read()), Setting,
  Settings, ConfigError, Environment.
- sched/worker_pool.h WorkerPool, WorkerPoolConfig, ClassGates, WorkContext, FrameType::foreground.
- listen/backoff.h Backoff (free failures, initial, max, forget; wait, failed, succeeded, size).
- listen/stream.h Stream async_read_some, send, start_tls_unanswered, close, abort, peer,
  get_executor; listen/listener.h StreamListener, ListenerConfig (start_tls), Certificates.
- Test: fixtures/stepped_time.h; next, fixtures/sim/nodes.h (SimNodes), dannan::sim_checkers and
  DANNAN_TEST_TEMP_ROOT for its step 3.
- Context::time().after() routed through the sim's schedule (on 0.9; must not break).

Kvasir's steps: 2a one node in memory (now), 2b state in Dagda, 3 the commit protocol in the
simulator, 4 the cluster at decided and replicated, 6 ephemeral and persisted, splits and merges.

Needs, by item:
- 1: as recorded. Blocks step 3. Proof: ranges commit through change_deltas() in SimNodes with
  state-wiping restarts, and catch_up restores readiness.
- 2: operation ids with an applied window (Kvasir also carries its own transaction ids); the
  commit notice to wake readers and folding; catch_up name enumeration for the range map.
  **The notice must report gaps, never be silent.** Blocks step 4. Proof: a notice reaches every
  replica of a range in SimNodes, and a lost one is repaired by anti-entropy.
- 3: a notice when a slot's set changes (old set, new set, version) and a way to catch new
  replicas up before transfer() commits (Kvasir's manifest plus log tail): a pre-transfer hook or
  a "ready to transfer" step. Blocks step 4. Proof: a 1, 3, 5, 3 node SimNodes run keeps every
  range at its replica count with no lost commit.
- 4: called from the node program. Proof: a double founding refused with a Status line naming
  the other cluster.
- 5: a draining notice, so Kvasir moves its proposer stickiness away first.
- 6: hlc stamps for commit timestamps and admit() at every receiver. Proof: the one, two and
  five node test, and snapshot reads never seeing a commit stamped past an admitted bound.
- 7: checks catalog loaded, manifest head caught up (Dagda's signal), Dagda store ready;
  kvasir-node refuses clients (1053 or the handshake) while not ready. Blocks 2b. Proof: a node
  restarted in the simulator refuses clients until caught up.
- 8: relied on so a slow replica never stalls a commit past its deadline.
- 9: unpadded base64 for the PHC text.
- 10: the hard bound on Backoff (keyed by account and address today, sign_in.cpp:246-254).
  Proof: a flood of distinct names leaves memory bounded and a real name's penalty intact.
- 11: make(password) -> PHC; verify(phc, password) with constant work and a decoy;
  needs_rehash(phc). Replaces accounts.cpp:55-107. The caching_sha2 fast-path verifier stays
  Kvasir's. Proof: accounts tests pass on Dannan's helper, an absent name takes the same work.
- 13: the ack-level enum with text names, parsed from config and SET; the callback vocabulary.
  Proof: Kvasir's enum deleted.
- 14: Kvasir's own settings ride through the same config Reader; a second listener (MySQL,
  start_tls in place) on the helper's io context. Proof: kvasir-node is a hosted system in a
  Dannan node with status, readiness and drain. Kvasir moves to Dannan's logging with it.
- 15: welcome for its proposer takeover backoff. 16: transaction ids at step 4.
- 17: sim World, SimNodes, SimNetwork, SimDisk, stepped time, fuzz Registration and driver (its
  packet reader, lexer, parser and prepared-statement targets), and a fake Stream (in memory,
  scripted reads, captured sends, start_tls_unanswered). Proof: Kvasir links dannan::testing from
  the installed package.
- 18: ready() false in a real node; per-operation progress under faults.
- 20: certificate expiry, through the node helper.
- 25 (DNS): registers the MySQL service. Proof: a client resolving the service name reaches a
  ready node after any one node stops.
- 22: one register per table (its ranges) and one per range, so ranges are names in agreement.
  Ranges split near 64 MiB (not ruled): 1 TB is about 16,000 ranges, 2 to 4 times that with
  indexes; at 10 to 50 TB, hundreds to over a thousand a slot. Splits can come many times an hour
  while a table loads. Moves: new replicas caught up before transfer() commits, one transfer
  round per move; writes to a moving range pause at most about 1 s, never in proportion to data
  or names; reads at decided may use the old set; never every range at once. A fingerprint
  change is fine if old-fingerprint requests are refused with a retry hint while it rolls.
- Not used: 12, 19.
- Not on the list: N1, atomic commit across replica sets (the cross-slot commit): needed at step
  4 for any transaction touching two ranges; intents per row in the range's value, the hosted
  system's conflict rule, an hlc commit stamp. **Asks that it come no later than the third minor.**

Constraints: completions posted to the caller's executor; one strand per MySQL connection;
DeltaChange runs on agreement's strand and never waits on a store. Never blocks an io thread
or a connection strand (Argon2id on the WorkerPool). Every call has a deadline. Range values to
256 MiB (rare whole), deltas from a 10 to 20 ms group commit; packets 16 MiB, max_allowed_packet
64 MiB; ancestry at most 16 ballots. Work classes honoured end to end: client statements and
the replication a commit waits on are foreground; catch-up and scans loader; folding,
collection, index builds and backups speculative. Time injected everywhere.

Reconciled, and open:
- **Atomic commit across replica sets moves into the plan** as item 26. Two consumers name it
  their largest block (Mimir every multi-key write, Kvasir any two-range transaction). Proposed:
  the third minor, after 3, since commits across sets need sets that move correctly. The shape
  starts from Mimir's proposal with Kvasir's corrections (TODO/ACTIVE.md's former 0.10.0 item 4).
- 2's notice reports gaps (at most once, never silent), with anti-entropy repairing them.
- 3 gains a set-change notice (old, new, version) and a pre-transfer step where the hosted
  system catches new replicas up before transfer() commits.
- Paging: Kvasir's hundreds to thousands of names a slot at scale reach max_names too; the paged
  transfer and catch-up under Mimir's answer covers both.
- 5 gains a draining notice to hosted systems (with the health advert, item 7).
- 14 takes hosted settings through the same Reader and lets a hosted system run its own
  listener on the helper's io context.
- 17's fake Stream supports start_tls_unanswered.
- 22: an agreed table change, old-fingerprint requests refused with a retry hint while it rolls,
  per-range handover within about 1 s, never all at once.

### Louhi (2026-11-20, pinned 0.9.2)

Used today, not to break:
- agree/: table().slots; change(space, slot, {name}, replicas, version, change, Reduce{},
  deadline, executor, done) (admit/agreement.cpp:50-52); slot_of; Slot; Name; Outcome committed,
  unknown, why, result; caspaxos.h Bytes, Change, Changed values/result/repeatable.
- net/codec.h Reader (u8..u64, string(max), fixed<N>, finish), Writer, DecodeError, Bytes (about
  150 uses).
- identity: random_bytes, hex, unhex, NodeId parse/to_string, Secret and reveal(),
  durable_replace_file, SigningKey::from_pkcs8 and load_cluster_key (through other headers).
- encoding/percent.h percent_decode, percent_encode.
- listen/: StreamConnector and connect(... optional<ClientTls>, deadline, cb), close_all,
  ConnectorConfig.name, ClientTls trust/alpn, ClientTrust; Stream get_executor, send, close,
  abort, when_finished, used as a Beast AsyncStream; StreamListener start, port, stop_accepting,
  close_all, ListenerConfig name/host/port/certificates/tls.alpn/max_connections;
  CertificatePaths, Certificates; Prefix parse/address/length/contains, reached through limits.h.
- contract/time_source.h TimeSource now/wake_within, WallClock, TimePoint, system_wall_clock,
  steady_time_source; thread_safety Mutex, Lock, annotations.
- sched/retry.h RetryPolicy, **brace-initialised positionally** (agent.h:90,
  service_token.cpp:27): its field order is API; RetryState failed, succeeded,
  consecutive_failures, last_failure, failures_in_window.
- status/log.h ScopedLogger by value, console_sink, null_sink, LogLevel, LogFormat, parse_*.
- context.h Nodes (passed to Lir's client), retire, advertise("louhi", {}), withdraw; node.h
  NodeConfig (state_path, key, log, listen_host, advertise_host, port, bootstrap, node_name,
  failure_domain, heartbeat, dead_after, telemetry_interval, version), Node, Context nodes,
  identity().id(), logger(name), net::Endpoint; config/ Reader and Settings (toggle, number,
  list, text, path_of, choice, refuse, positional, warnings).
- Tests: stepped_time.h (SteppedTime, SteppedWall), temp_dir.h, sim/agreement.h (World, Cluster,
  NetworkFaults), linked as dannan_sim_checkers (no dannan:: alias in a fetched build).
- CMake: FetchContent at 0.9.2 with OVERRIDE_FIND_PACKAGE; dannan::dannan PUBLIC.

Needs, by item:
- 1: serve("louhi", "louhi", store) on each node before Dagda's durable store. Blocks the API onto
  louhi::admit, then placement (its step 6). Proof: Louhi's admit simulation cases pass on SimNodes
  with a node restarted, wiped and caught up.
- 3: agreement().serving(space, slot) -> ReplicaSet, or change() finding the set itself. Blocks
  real use of every register. Proof: five SimNodes that lose and gain a node keep committing
  Louhi's revisions with no caller-side set handling.
- 2: operation ids optional for Louhi (its changes recognise their own mark).
- 23: from its command.h; Louhi moves onto it.
- 24: the server replaces api/http.cpp (one-shot exchange, body limit, read timeout, 413,
  Connection: close, plain and TLS listeners); the client replaces api/client.cpp: http:// and
  https://, bracketed IPv6, default ports, a body limit, one posted completion. Proof: api_http_test
  and the louhi-ctl tests pass unchanged.
- 7: register(name, check) plus a health advert read by placement; its checks: Docker answers,
  memory and user namespaces enforced, storage bounds.
- 5: louhi-node --retire calls the cluster act.
- 14: replaces louhi_node.cpp's scaffolding.
- 16: TaskId and RequestId.
- 13: runtime/driver.h's Completion, Result, Error, Deadline, Done; **a typed error kind
  (unavailable, refused, not_found, timed_out, malformed)** that Louhi branches on.
- 15: two callers. 6: nothing Louhi-specific. 25: later, for its service discovery (step 10).

Constraints: one io_context on several threads, a strand per component; completions posted;
nothing blocks an io thread. API writes 5 to 30 s; agreement changes within the API call's
deadline (1 s per attempt in tests). Register names under 128 bytes; values about 5 KiB to
240 KiB; one name per change; about 400 names a slot later; tens of nodes.

Reconciled, and open:
- 3: two consumers (Mimir, Louhi) want change() and read() to find the serving set themselves.
  That is the shape: the set and version become optional, with serving(space, slot) for those
  who want to see it.
- 13: Outcome gains a typed error kind (unavailable, refused, not_found, timed_out, malformed),
  beside why.
- 17: dannan_sim_checkers keeps its name, gains a dannan:: alias, and is part of dannan::testing.
- RetryPolicy's field order is kept; unbounded() is a named constructor, not a reordering.
- 24: the client speaks plain http:// as well as https:// (Louhi's ruling of 2026-11-15).

### Lugh (2026-11-20, develop pins 0.9.1; P8.0 moves to 0.9.2)

Used today, not to break (T: tests only):
- contract/thread_safety.h Mutex, Lock, IoMutex, GUARDED_BY, REQUIRES; time_source.h
  TimeSource::now, TimePoint, WallClock::unix_ms, steady_time_source, system_wall_clock.
- identity/crypto.h Bytes, sha256, hex, random_bytes, constant_time_equal, hmac_sha256,
  hkdf_sha256_secret, aes_gcm_seal, aes_gcm_open_secure, cleanse; secure_memory.h Secret (span,
  random, reveal, ==), SecureBuffer, unlocked_secure_allocations, harden_process; signing.h
  SigningKey (generate T, from_pkcs8, to_pkcs8 T, algorithm, public_key, sign), es256 and
  ed25519, algorithm_name, algorithm_of_public_key, verify, verify_rs256; argon2.h Argon2Costs,
  argon2id; durable_file.h durable_replace_file, durable_create_file.
- encoding/percent.h percent_decode (Decoding::form, strict), percent_encode, PercentSet form,
  unreserved, printable_ascii, .encode().
- net/codec.h Reader, Writer and BasicWriter<SecureBuffer> (raw, take), DecodeError.
- config/config.h Reader (settings, config_variable, default_path, environment, read(args),
  describe()), result warnings/path/positional/text()/refuse(), Settings, Setting, ConfigError,
  Environment::get, process_environment.
- status/log.h LogSink (enabled, write), ScopedLogger(sink, name) info/warn/error/debug,
  ConsoleSink, console_sink, LogLevel, LogFormat, parse_*.
- listen/: backoff.h Backoff, Rules{free_failures, first, longest, forget_after}, wait, failed,
  succeeded; certificates.h Certificates(CertificatePaths{chain, key, client_ca}), reload (T);
  listener.h StreamListener, ListenerConfig (inherited, tls.minimum, client_certificates, alpn,
  handshake_timeout, start_tls, max_connections), TlsVersion::tls13, ClientCertificates;
  stream.h Stream peer, client_certificate, set_idle, close, abort, get_executor,
  async_read_some/async_write as a Beast stream, is_tls, start_tls; connector.h StreamConnector
  (strand_per_stream), connect, ClientTls{trust, certificate, minimum}, ClientTrust;
  limits.h Prefix{address, prefix}, forwarded_for(header, prefixes, peer).
- sched/retry.h RetryPolicy, RetryState (on a branch).
- T fixtures: SteppedTime, SteppedWall, ChildProcess(args, log, ChildOptions{environment,
  input}) read_all/wait/terminate/signal, free_port, TempDir::path, DANNAN_TEST_TEMP_ROOT.
- Build: DANNAN_BUILD_TESTING=OFF, DANNAN_BUILD_TOOLS=OFF, dannan::dannan, and FetchContent with
  OVERRIDE_FIND_PACKAGE so Dagda's find_package(dannan) takes the fetched one. Keep that working.

Needs, by item:
- 1: not directly (Dagda's durable store).
- 2: TOTP last step and recovery codes (a step accepted only if later), key rotation, POSIX
  number allocation (a counter refused on a minority). Calls: change(name, key, fn(current) ->
  next or refuse, deadline, executor, done(Outcome with unknown)) and value(name, key);
  unknown is treated as refused, failing closed. Blocks P8.3 and P8.6, no stand-in (the maintainer: no
  interim). Proof: two nodes offering one TOTP step at once see exactly one accepted, and a
  minority side refuses.
- 4: lugh-node init calls the founding act, which makes lugh/scope's genesis. Blocks P8.1. Proof:
  a second founding refused with the Status line, and a node joining by name takes the genesis.
- 5: --retire through the node helper; secrets readers removed in Dagda's catalogue.
- 6: readiness falls past the bound. Proof: a skewed node stops answering /ready.
- 7: checks spaces caught up (Dagda), custody and keys open, hashing backlog under a bound;
  replaces health_routes.cpp's /ready. Proof: an overloaded hashing pool turns /ready false and back.
- 9: base64 padded standard, base64url unpadded, base32 (RFC 4648, upper case out, upper-only
  decode), hex; **also utf8_length, valid_utf8 and printable (refuses C0, DEL, C1)**. Prefix:
  host bits refused at parse, IPv4-mapped unmapped; contains() per request. Proof:
  lugh/network/cidr.* goes.
- 10: take(key, Policy{burst, per_minute}, now) -> optional retry_after (zero means unlimited),
  a key bound **dropping full buckets first**, TimeSource-driven, retry_after rounded up to at
  least 1 s; keys address, session, account. Proof: http/rate_limiter.h goes.
- 11: costs, 16-byte salt, 32-byte hash; make; constant-time verify; needs_rehash(costs); a decoy
  spend on the WorkerPool; out-of-bounds costs refused before work; binary fields kept in Lugh's
  record. Proof: authn/password_hash.* goes.
- 12: SHA-1 and HMAC-SHA-1 for TOTP (OpenSSL called directly today).
- 13: Completion posted once, never inline, with late() when the deadline passes and a later
  finish dropped; Outcome<T>{Fault, message, value} with success, failure, from; **Fault none,
  not_found, conflict, duplicate, invalid, refused, failed**; Deadline in ms; Done<T> a
  std::function; Executor any_io_executor. Proof: lugh/core/completion.h and outcome.h become
  aliases.
- 14: P8.1 waits on it rather than writing its own: Lugh's settings beside the standard ones, a
  public HTTPS listener's certificate paths, signals and drain, --version, init (founding),
  --retire, custody, LogFormat. Proof: lugh_node.cpp's main is the helper plus Lugh's root.
- 15: TokenSource and the key source. 16: record ids. 17: everything marked T plus
  RecordingSink with scope and text(). 20: its public TLS certificate.
- 24 (HTTP and JSON), starting files: lugh/http/http_server, router, forms, client_address,
  static_site, health_routes, http_types.h, api_json.h, rate_limiter.h, and client/https.
  - Generic, moving: the keep-alive loop over StreamListener; the middleware chain; route
    matching with {param}, 404 and 405, percent-decoding; the in-flight cap; the keyed limiter;
    forms, cookies and the __Host- Set-Cookie builder; X-Forwarded-For resolution; the static
    SPA (ETags, 304, CSP script hashes); /health and /ready; the JSON helpers; the HTTPS client.
  - Staying Lugh's: Caller and CallerResolver, scopes, the Bearer realm 401, 403 scope checks,
    the phr step-up rule, the Lugh-Client-Cert-* headers, the Server header, back-channel
    logout. The fixed security headers (HSTS, CSP, nosniff, Referrer-Policy) become the core's
    settable defaults.
  - Bounds kept: request body 64 KiB (413, connection closed), form body 8 KiB; idle 30 s
    re-armed per read; TLS handshake 10 s; server TLS 1.3 only, ALPN http/1.1; 256 handlers in
    flight, then 503 with Retry-After 1, never queued; limits burst 30 then 30 a minute per
    address on open routes, burst 120 then 600 a minute per caller, 429 with Retry-After; static
    site 4096 files and 64 MiB; client answer body 64 KiB, TLS 1.2 minimum, Connection: close,
    no redirects, GET POST PATCH PUT DELETE.
  - Gaps to close in the core: an explicit header and target bound (Beast's 8 KiB default today),
    a total per-request and write timeout, max_connections on the HTTP listener.
  - Proof: Lugh's generic files and client/https.cpp become aliases and its tests pass.

Constraints: everything asynchronous takes a deadline and the caller's executor and posts there;
handlers posted to the router's executor, middleware next() dispatched on the stream's strand.
Argon2id, token verification and LDAP binds never on an io thread. Scopes at most 1024 bytes;
records at most 64 KiB on D6.

Reconciled, and open:
- **13: one fault kind for the stack.** Lugh (none, not_found, conflict, duplicate, invalid,
  refused, failed) and Louhi (unavailable, refused, not_found, timed_out, malformed) both branch
  on one. Proposed union: none, not_found, conflict, duplicate, invalid (covers malformed),
  refused, unavailable, timed_out, failed. Completion takes Lugh's late() rule. To confirm with
  both.
- **10: one keyed map for three callers.** Per-call Policy{burst, per_minute} (Lugh) also serves
  Mimir's fixed capacity and rate; take() answers an optional retry_after rounded up to at least
  1 s. Eviction past the bound: buckets that have refilled (full) first, since forgetting them
  loses nothing, then least recently touched (Kvasir's and Mimir's case). Backoff gets the same
  bound.
- 9 gains utf8_length, valid_utf8 and printable.
- 24's starting point and bounds are Lugh's, plus the three gaps it names; the security headers
  are settable defaults.
- The build must keep DANNAN_BUILD_TESTING=OFF and DANNAN_BUILD_TOOLS=OFF working, and
  FetchContent with OVERRIDE_FIND_PACKAGE, so one fetched Dannan serves Lugh and Dagda together.
- 2: Lugh's register calls (change with a refusing fn, value, unknown failing closed) are the
  register layer's user-facing shape, on top of Mimir's recovery and Kvasir's gap reporting.

### Macha (2026-11-20, port/dannan-dagda on 0.9.2)

Used today, not to break:
- node.h: Node start, stop, request_stop, cancel_outbound_calls; routes, nodes, telemetry, events;
  readiness (hold_until_marked, has, failed, error); threads().statuses, inbound().state,
  client_stats, time().executor. NodeConfig version, planes, telemetry_layout and fields,
  inbound_journal_fields, gateway, observation_interval=0, log, the four memory reserves,
  memory_limit_bytes=0, custody, connectivity, *_no_progress and *_stall_notice at 0
  (src/cluster/dannan_bridge.cpp:30-84, cluster.cpp:125-144).
- context.h: RosterChange; Nodes snapshot, roster, self, directly_reachable, all_known_reachable,
  watch, advertise; Routes add, retire, call (record or endpoint); Telemetry local, all,
  persisted, persist, views; Identity.
- net/: routes.h MessageType, first_user_type, Registration (notification, ordered, on_session),
  Lanes, Frames, RouteTable::with_dannan_types; message.h; client.h CallHandle cancel, abort,
  promote, idle_for, CallOutcome, ClientStats, WorkStats, Timing; outbound.h max_pending_replies,
  max_outbound_messages.
- membership/node_record.h NodeRecord, adverts, inbound_capable, capability_inbound, u64_advert,
  IdentityReset.
- identity/: ClusterKey derive, load_cluster_key; KeyCustody create, exists;
  KeySource::systemd_credential; Hash256, NodeId, to_string; Secret (tests);
  durable_replace_file.
- sched/: work.h FrameType, WorkClass, work_class, frame_for, frame_type_name, Waits,
  default_waits, local_waits, includes, may_enter, ThreadSafety; retained_memory.h
  RetainedMemoryLedger, Lease, MemoryOwner.
- status/: LogSink, LogLevel, ScopedLogger, ConsoleSink; Telemetry, field::*, Phase; NodeEvents,
  ActivityClocks, Observations.
- supervise/: StartupProgress, StartupStallGate, SupervisedThreads.
- contract/time_source.h TimeSource, WallClock, wake_within.
- reach/: UpnpGateway, Inbound, InboundState, PublicConnectivityStatus.

Gaps to close before Macha deletes its copies (the one rule: no loss of function):
1. durable_file: public durable_create_directories and durable_sync_directory
   (catalogue_hints.cpp:215, metadata.cpp:1963); keep the FileSystem& overload (Macha's
   DirectorySync hook); **a mode for made directories**: Dannan makes 0700, Macha uses the umask
   and the cutover's live paths depend on it. Item 19 is wanted.
2. retry: a default `now` (subsystem_supervisor.cpp:277, maintenance.cpp:616); Macha can pass it.
3. log, the largest: Macha's process-wide static Log with an atomic-mask enabled() before any
   string is built (about 40 hot sites), an emit that bypasses the filter, a runtime
   set_logger swap (about 25 tests), capture sinks for tests.
4. crypto: object_id (about 20 uses); subkeys by ClusterKey::derive (a Macha shim);
   random_node_id is NodeId::random; empty-AAD handling to verify identical.
5. codec: parity; Reader(Bytes&&) deleted and oversize bytes() throws length_error, no Macha site
   hit.
6. supervised: about 40 free-function sites, some before the node exists (dannan_host.cpp:95);
   Node::threads() and readiness() should be const (cluster.cpp:627-643 const_casts).
7. subsystem: shapes match; the park and fault-sink loop's parity with
   subsystem_supervisor.cpp:171-294 unverified; proof, a faulting plugin parked against both.
8. observation: a process registry (about 30 uses); elapsed_us(time_point); ObservedDuration on a
   histogram handle (Dannan looks the series up per record); one shared writer (Macha disables
   the node's recorder, cluster.cpp:142).
9. walk: must_stop(now) and WorkContext::expired(now) without a default; Macha can pass now.
10. placement: rendezvous, by_capacity(Scheme{"macha/data-fallback/v7",
   "macha/data-extra-domain/v7", capacity_advert}), logical_capacity, order_commit_replicas,
   **byte-identical to Macha's placement.cpp:260-379**, or existing placements move. Proof:
   Macha's placement vectors run against Dannan's.
11. config: lists of maps (storage.data.backends[{path, limit, reserve_free}]) and nested
   hydration engines; size units (4M, KiB), octal modes, doubles, tristate auto; [v6]:port
   endpoints; *_ms durations; **unknown keys refused, not warned**; a cross-field validate with
   warnings. The schema stays Macha's.
12. contract: TimeSource's now_waits and now_safety constants; WaitGuard is static and
   process-wide in Macha (work.cpp:18-57) and per node in Dannan (catalogue.cpp:1457,
   metadata_manager.cpp:1505, :2013 would need the node).
13. worker pools: WorkerPool covers few of Macha's: long-lived loop workers (ingest, hydration,
   FUSE data and durability), cancelling a running task (book conversion), a lazy pool with
   blocking submit, futures and peak stats (extent pool), the FUSE broker's twelve
   operation-class queues in one WorkClass with a weighted spread, per-task deadline and
   cancel, an EAGAIN admission cap and a promise the kernel thread waits on, queue-wait max and
   peak stats (HTTP lanes).
14. HTTP core: single-range requests (206, 416); streaming bodies from a pull source and a
   zero-copy resident path; keep-alive with max requests, idle timeout, pipelining;
   max_request_bytes 413 and close, header cap, max_connections; a threadless parked handler
   re-run by a waker; two lanes (control, data) with 503 and Retry-After queue bounds; inline
   routes answered on the reactor; a work-class activity callback; gzip of in-memory bodies;
   Bearer with exemptions; OPTIONS 204; CORS *; client IO timeouts, slow-request logs,
   reactor-stall diagnostics; always Content-Length, no chunked bodies; JSON keeping int64 and
   uint64 distinct.
15. RPC: Routes unbind waits for calls in flight and answers 501 for an unbound type; **a thrown
   handler answered as a 500 reply** (message_routes.hpp:45-49); CallWatch{no_progress,
   stall_notice}, with Macha's stall journal line (peer, message, age) and transient-failure
   classification, unverified in Dannan.

Constraints: FUSE kernel threads only enqueue and wait on a future up to min(class timeout,
15 s); broker, data and HTTP lane workers make synchronous calls that block on a reply future
(cluster.cpp:328), so Routes::call itself never waits. Maintenance runs on Dannan's TimeSource
and WallClock. Completions and the roster watch run on a strand of time().executor(). The
metadata_notice handler runs on_session and must not wait; server workers dispatch handlers
synchronously and those may do I/O. Node-wide no_progress and stall_notice are 0; Macha polls
idle_for (control notice 5 s, data 120 s; control no-progress 30 s). cancel_outbound_calls
fails calls at once. Never block: on_session handlers, roster and advert watches, completions
on io executors, WorkerPool::submit, Routes::call, Nodes reads, CallHandle operations,
LogSink::enabled. Macha's own risk: its stderr sink takes a mutex on an io thread; AsyncSink
exists and Macha should use it.

Reconciled, and open:
- **The process-global question.** Macha's log facade, supervised threads, observation registry
  and WaitGuard are process-wide; Dannan's rule (CLAUDE.md, the maintainer) is dependency injection with
  the io context the only process global. Both rules hold if Dannan gives instance types that
  are usable before and outside a node, with the fast paths Macha needs, and Macha's static
  facade becomes Macha's own thin holder over one instance (its composition, not a second
  implementation). So Dannan adds:
  - log (item 28): LogSink::enabled(level) as an atomic level-mask check, no allocation; an
    emit that bypasses the filter; a swappable sink holder type (set, enabled, emit) a
    composition root can own; capture sinks in dannan::testing.
  - supervised (6): SupervisedThreads constructible with no node; threads() and readiness()
    const on Node.
  - observation (item 29): an Observations instance a process can own and hand to the node
    (one shared writer); elapsed_us(time_point); a histogram handle with ObservedDuration
    recording without a per-record lookup.
  - WaitGuard (12): a guard instance not tied to a node; TimeSource gains now_waits and
    now_safety constants if they are wait declarations, to be read.
  Macha confirms this shape, or it goes to the maintainer as a conflict between the one rule and the DI
  rule.
- **durable_file (item 19 becomes firm):** public durable_create_directories and
  durable_sync_directory, and a directory mode parameter defaulting to today's 0700, Macha
  passing its umask-respecting mode. A test that a made directory's mode follows the parameter.
- **config (item 27):** lists of maps, nested sections, size units, octal modes, doubles,
  tristate auto, bracketed IPv6 endpoints, *_ms durations, unknown keys refusable (a reader
  option, since Lugh and Louhi read warnings today), and a cross-field validate hook with
  warnings. Lugh, Louhi, Mimir and Kvasir use the reader too, so each gains, nothing changes for
  them by default.
- **placement:** Macha's placement vectors become Dannan tests before Macha switches; any byte
  difference is Dannan's to fix (the one rule).
- **RPC (item 30):** a handler that throws is answered with a 500 reply, not a dropped session;
  the stall journal line and transient-failure classification checked against Macha's and
  added where missing.
- **worker pools:** stay Macha's for now. The FUSE broker, the extent pool and book conversion
  have one user. Long-lived supervised loops already exist in supervise/. If a second project
  needs a weighted multi-queue inside one class, it comes down then.
- **HTTP (24):** Macha's needs join Lugh's and Louhi's: ranges, streaming bodies and a zero-copy
  path, keep-alive limits and pipelining, deferral by waker, two lanes with queue bounds, inline
  routes, the activity callback, gzip, Bearer with exemptions, OPTIONS and CORS, slow-request
  and stall diagnostics, Content-Length always, int64 and uint64 kept distinct in JSON. The core
  is the union; each project's auth and routes stay its own.
- crypto: object_id stays Macha's unless a second project needs it; the empty-AAD case becomes
  a Dannan test.
- retry, walk: Macha passes `now`; no change.

### Confirmations

- Lugh (2026-11-20): agrees to the one fault enum (none, not_found, conflict, duplicate, invalid,
  refused, unavailable, timed_out, failed), Completion's late() rule and Outcome<T>; maps
  unavailable and timed_out to 503 with Retry-After in its API. Agrees to the keyed map: full
  buckets dropped first, then least recently touched; keys sign-in by account and address
  together plus a per-address key.
- Louhi (2026-11-20): agrees to the fault enum, malformed becoming invalid; asks that the contract
  text say invalid covers both the caller's input and a reply that cannot be read, the message
  saying which side was at fault. Agrees to item 3's shape (set and version optional, serving()
  kept for status). Notes RetryPolicy's order kept, unbounded() named, the sim_checkers alias.
- Macha (2026-11-20): the holder shape keeps every call site's behaviour, on four conditions on
  Dannan's instances, now part of items 28, 29 and 6:
  1. usable before anything is set: a default sink (stderr at INFO, or AsyncSink) answers enabled
     and emit before the root sets one, the mask check lock-free from the first call (Macha logs
     during config load and plugin dlopen);
  2. one instance per process across shared objects: nothing on Dannan's side keeps a per-.so
     copy (no function-local static in an inline header), since Macha's dlopen'd plugins log
     through macha_core;
  3. swappable at runtime without a race: a swap atomic for concurrent enabled and emit, an old
     sink never destroyed while an emit is in it (shared_ptr handoff);
  4. emit bypassing the filter, callable from FFmpeg's and libtorrent's own threads.
  WaitGuard's record and throw modes, which tests switch, are kept. Firm: Macha passes its
  directory mode, chooses unknown-keys-refused, takes the 500 for a thrown handler, the placement
  vectors become Dannan tests first, worker pools stay Macha's.
  Flag for the maintainer: Macha's holders are process globals in Macha. If "the only process global is the
  io context" binds every project, removing them means threading a logger, a supervisor, an
  Observations and a guard through hundreds of constructors.
- **the maintainer (2026-11-20), on Macha's flag:** "Surely Macha would thread a logger and the Dannan
  instance." So no holders: Macha's components take a logger and the Dannan instance (the node's
  Context) by injection, and reach supervised threads, observations and the wait guard through
  that Context. For Dannan this means:
  - Context exposes everything a component needs from these four: logger(name), threads(),
    readiness(), observations() and the wait guard, each const where it reads.
  - The logger is constructible before any node, from a sink the root owns, so config load and
    plugin dlopen log through it; the root hands it to the node and to plugins through the host
    entry. Conditions 1, 3 and 4 above still apply to the sink (lock-free enabled from the first
    call, race-free swap, filter-bypassing emit from foreign threads); condition 2 holds by
    construction, since nothing is static.
  - Items 28 and 29 drop the "holder" type; Dannan ships the instances and Context accessors only.
- Macha (2026-11-20): every site can take a logger and the Context except FFmpeg's log callback
  (src/media/ffmpeg_log.cpp). av_log_set_callback takes one process-wide C function pointer with
  no user data, called from FFmpeg's codec threads. The media engine, built with its logger,
  stores it in one file-local atomic shared_ptr when it installs the callback and clears it at
  shutdown: the one static in Macha, imposed by FFmpeg's API, documented as such. Taken as the
  obvious exception (a third-party C API, not a design choice); reported to the maintainer, not asked.
  libtorrent needs none: its alerts come through the session object, built with the logger.

### Lir (2026-11-20, the rewrite's plan; in full at ../lir TODO/2026-11-20-requirements.md, "To Dannan", feature/rewrite acc0887)

Will use at 0.9.2: Nodes watch, watch_advert, snapshot, placement with only_advertising, both
advertise forms, partition, absent_since; Routes add, bind_deferred, call; Channels notify and
stream, the pushed subscribe; Identity id, durability_epoch; Budget, Time, Telemetry, hlc(),
status, readiness, threads; net/codec.h, net/fanout.h; listen/ listener, Stream, beast.h,
connector.h, backoff.h, limits.h; config/config.h. Not agreement.

Needs, by item:
- 17: blocks every Lir test past the codec. A test must open a client connection to a chosen
  node inside the sim world (a sim listen::Stream or an injectable accepted stream), and the
  world must hold Dagda's nodes too. Without the client seam it does not serve Lir.
- 8: subscribe(..., deadline, per_peer, done) with Pushed naming slow peers; SUBACK goes once
  every non-slow peer holds the interest.
- 6: through Dagda for retained values; hlc's physical time for message expiry.
- 24: routing, body limits, a streaming SSE response with backpressure, a WebSocket upgrade
  handed over as a Stream, the client address behind proxies, keyed limits, Boost.JSON. Proof:
  the claim board's three endpoints.
- 25: A and AAAA for the ready nodes advertising lir.endpoints, TTL about 5 s; SRV _mqtt._tcp and
  _secure-mqtt._tcp optional (common MQTT clients do not resolve SRV).
- 7, 10, 13, 14, 16 as in Lir's file.
- Not on the list: **selective interest at scale.** At 50,000 filters a node every interest
  advert coarsens to the empty prefix (fanout.h:73-76), so every publish reaches every node:
  correct, but no routing. Lir needs a representation that stays selective at that size (a
  Bloom filter or hashed prefix set, paged or larger); the shape is Dannan's.

Constraints: calls from strands of the io executor; publish and match never wait. SUBACK 1 s or
the per-peer bound; take-over call 2 s. 4 to 16 nodes, 10,000 sessions and 50,000 filters a node,
10,000 messages a second cluster-wide.

Reconciled, and open:
- **Item 31, selective channel interest.** It cannot stay an advert: Dagda's ruled design keeps
  adverts for slow facts under 1 KiB, and interest at 50,000 filters changes with subscriptions
  and needs tens of KiB even as a Bloom filter (about 60 KiB at a 1% false-positive rate). So
  interest moves off the node record onto its own exchange over CONTROL: a per-channel summary
  (Bloom filter or hashed prefix set) with deltas, pushed to peers, versioned per node, and the
  pushed subscribe's Pushed evidence computed from it. A false positive costs one wasted
  delivery, never a lost one. Mimir's pub/sub by prefix uses the same path. Shape to design;
  third minor unless Lir's tests need it sooner (Lir's call).
- 17 gains a client seam: an in-world Stream a test opens to a chosen node, and Dagda's nodes in
  the same world.
- 8: subscribe takes the per-peer bound; Pushed names slow peers separately from missed ones.
- 24 gains SSE with backpressure and a WebSocket upgrade handed over as a Stream.
- 25: A and AAAA required, SRV optional, TTL configurable (Lir 5 s).
- Lir (2026-11-20): confirms 31, 17, 8, 24 and 25 as recorded. 31 in the third minor is fine;
  Lir's harness tests run at tens of filters. Keep: Pushed computed from the interest exchange
  still says which peers hold a just-added filter (the SUBACK hold rests on it). 17's in-world
  client Stream with Dagda's nodes in the same world unblocks Lir's step 2 and is the item Lir
  waits on most.
- Dagda (2026-11-20), item 13: agreed. "durable" stays Dagda's own value in store::Durability, not
  a fifth member of the stack's enum (the maintainer, 2026-11-15, already ruled it stays beside the four). It
  means on disk on every available member of the object's set, never waiting for a known-dead
  member, never undone, evidence naming who is pending; replicated is a majority. A caller asking
  the stack's "replicated" gets Dagda's replicated; durable is never offered under the stack's
  names. Confirms the registered retire check, the fixtures forwarding for one minor and
  agreement.h's ABI held through the first minor. **The ack-level open question is closed.**

## Shapes agreed while building the first minor

### Item 17: a carrier under listen::Stream (Lir, Kvasir, Lugh)

listen::Stream's byte operations go through a small type-erased Carrier: SocketCarrier (today's
TCP and TLS, unchanged) and MemoryCarrier (one end of an in-memory pipe whose deliveries are posted
to a given executor, the sim World's io_context in a simulation). Stream's public API is unchanged.
- StreamListener::inject(executor, peer = 127.0.0.1) returns the client end of a pipe whose server
  end reaches on_stream through the listener's own accept path and limits; peer() reports the
  given address (Lugh: rate limits, back-off and X-Forwarded-For key on it); transport() "memory".
- TLS over a memory pipe is real TLS with the listener's certificates; the client end may present
  a client certificate (Lugh's RFC 8705 tests). start_tls and start_tls_unanswered behave as on a
  socket (Kvasir). set_no_delay is a no-op on memory.
- In-world dialling (Lir's lir::client): StreamConnector's in-world form uses a world directory
  from endpoint to listener, filled as SimNodes' nodes start their listeners. A stopped node's
  endpoint refuses at once; one cut off by SimNetwork times out on the world's clock under the
  connector's deadline. A client may have an address in the world so partitions include it.
- Faults on one connection: sever() (both ends reset, pending reads end in error), stall() (no
  bytes either way, nothing closes), resume() (flow restarts in order). Stopping a node severs
  every stream its listeners and connectors hold.
- The World stays shared: SimNodes does not own it, so Dagda's nodes run in the same world.
- Kvasir's fake Stream is the test driving the client end; its Transport seam can go.
- Cost: one indirect call per read and write on real sockets, measured with dannan-bench before
  and after; a direct path for SocketCarrier if it shows. Confirmed: Lir, Lugh; Kvasir to answer.
- Not now: datagrams (with the DatagramListener).

### Item 17: the fuzz export

fuzz/fuzz.h is installed. The library dannan_testing (alias dannan::testing, built always: it
needs no GoogleTest, so no option) holds the target registry and a callable smoke run over a
target (its seeds, mutations, noise, the empty input) drawn from SplitMix64, as the sim's Random,
so a seed replays alike on libc++ and libstdc++; and replay(argc, argv) and run_all(bytes) for a
consumer's two-line fuzz binary. It links dannan and dannan_sim_checkers, which gains the alias
dannan::sim_checkers in a fetched build (Louhi). The test fixtures' headers keep their paths
(dannan/test/fixtures/...), as Dagda asked, rather than moving under include/dannan/testing/.

### Item 24: the dashboards (Lir, Louhi, Mimir, Kvasir; the maintainer's instruction to the four)

- Static files from a configurable root (default the service's install), from Lugh's static_site:
  ETag and 304, bounded files and bytes; traversal refused (.., encoded slashes, symlinks out of
  the root); fingerprinted assets (a configurable name pattern) Cache-Control immutable for a
  year, index.html no-cache.
- SPA fallback, configurable: a GET matching no route and no file serves index.html; API prefixes
  (default /api/) never fall through.
- OpenAPI: routes() exposes the route table; check_openapi(document, routes) reports routes the
  document lacks and documented paths with no route, for a hand-written document (Louhi); an
  emitter builds the document from the table and per-route fragments for those who prefer it.
- Sign-in: password through item 11's credential helper (the service's own accounts), or Lugh
  through lugh_client behind a pluggable authenticator (Lugh's CallerResolver shape). Sessions are
  a stateless signed __Host- cookie (Secure, HttpOnly, SameSite=Strict): subject, scopes, expiry,
  HMAC-SHA256 under a cluster-key subkey, so any node checks it with no session store; an hour by
  default with sliding refresh; Bearer accepted for API calls; sign-in limited by item 10.
- The React code stays each service's. Sent to the four and Lugh 2026-11-20; answers pending.

Answers on item 24 (2026-11-20): Mimir, Louhi, Lir and Lugh confirm 1 to 3; Kvasir pending. Folded in:
- **Authentication per route** (Lir): a route names its authenticator (session cookie or bearer;
  the service's own, as Lir's MQTT grants; or none, the handler decides), and the cookie or bearer
  token reaches the handler. Root-level routes (/publish, /mqtt) are routes, never the fallback.
- **Revocation per request** (Mimir, Lugh): the cookie carries the principal's version as issued,
  and the core runs the service's authorize(subject, scopes, issued_at, version) -> allow or deny
  before every handler, answered from the service's memory (Mimir's ACL, lugh_client's version
  floor), no network call. Scopes are opaque to the core (Louhi's louhi:<project>:<action>).
- **Accounts behind an interface** (Lir, Mimir): password sign-in asks an AccountSource for a
  name's verifier: item 11's PHC credential from the service's YAML (Lir, Louhi), or the service's
  own check (Mimir's SHA-256 ACL hashes, Valkey's form). A Dagda register-map space can be a source
  later. Sign-in limited per client address (Mimir) through item 10.
- **Cookie name the service's** (Lugh): two services on one host never collide. Short expiry,
  refreshed in use. Lugh keeps its own stateful sessions for itself (admin-listed, revoked at once,
  phr and acr); the core's stateless cookie is for the dashboards.
- **Server-sent events** (Mimir, Lir): a streaming response on the connection's executor, never
  blocking; a heartbeat comment; a bound on the outbound queue (a slow client is closed); the
  session cookie accepted (EventSource sends no headers), and the stream ended when the session
  expires or authorize() turns to deny.
- **Asynchronous handlers** (Mimir; Macha's deferral): a handler may return pending and complete
  later from any executor, under a per-route deadline.
- **Bounded JSON bodies** (Mimir): a maximum body per route; a parse bounded in depth and size;
  413 or 400 in the service's error shape (Mimir {error, message}).
- **Configurable paths** (Louhi): the SPA fallback's API prefixes per service (Louhi /v1/), the
  OpenAPI document's path (Louhi /v1/openapi.json, Mimir /api/v1/openapi.json).
- Kvasir (2026-11-20) confirms item 17's carrier and item 24's core. Item 17 also states: (a) the
  client end runs a client TLS handshake trusting the listener's certificate; (b) the test sets
  peer() (per-source back-off); (c) faults from the client end: half-close, abort mid-packet, and a
  stall so the server's deadline fires; (d) bytes delivered in pieces the test chooses, to test a
  packet reader across arbitrary splits. Kvasir's tests with Connector/C++ over loopback TCP need
  nothing more. Item 24: the dashboard on its own configurable port with the node's certificates,
  off by default; accounts from a pluggable source (Kvasir's MySQL users, later Lugh's); the cookie
  key a cluster-key subkey taken from the node helper (item 14); a scope check per route (two admin
  actions: kill a session, drain a node). **Item 24's shape is confirmed by all five.**
- Item 27 gains Louhi's three Reader gaps (2026-11-20, from louhi-ctl's move onto the Reader): a
  value-kind flag given twice is refused as a usage error (list kinds take repeats; last-wins goes,
  in the CHANGELOG); a lone "-" is positional and "--" ends options; and a per-setting
  empty_is_unset for environment values, the default staying "empty is a value" unless the
  Reader's other users ask otherwise.

### Item 6: the clock rules, as agreed (Mimir, Dagda, 2026-11-20)

- Membership stamps every record change max(wall + 1, last + 1), as adverts already did.
- HlcClock::admit(stamp, peer) returns the receipt and AdmitVerdict {admitted, ahead,
  self_out_of_bounds} with a reason; merged=false means refuse, never apply. Dagda applies it when a
  peer's commit is offered for acceptance, never inside the merge (a merge stays a pure function of
  its heads); a refused commit is transient and Dagda retries on its own backoff. No re-entry notice.
- max_ahead's default: five seconds. The offset bound: two seconds.
- A clock probe (a new message type; an old node refuses it as unknown) estimates each peer's offset
  NTP-style on the heartbeat. A node whose offset passes the bound against more than half of the
  agreed roster, itself counted at zero, leaves ready, and hlc().now() throws ClockOutOfBounds (a
  std::runtime_error) at once, never waits. One or two nodes cannot reach that majority, with no
  special case by size.
- Agreement (Mimir's Mi-2): change() takes an optional HlcStamp, hlc().now() when absent, fixed for
  the change's life including retries, carried in prepare and accept, admitted by each acceptor and
  refused with "the proposer's clock is <n> ms ahead"; the Change gets it in a context argument.
  read() takes none. An agreement wire change: the cluster moves together.
- D-2 (tombstone maturity) is closed by these rules with no low watermark (Dagda).
- **0.9.3's ceiling fix folds into item 6, with the five-second max_ahead** (the maintainer, 2026-11-20:
  "Hold stamps after a crash"). The ceiling follows stamps issued as ruled on 2026-11-19 (issued +
  30 s, one write each 28 s while stamping; nothing written while idle; a clean stop writes it down
  to the last stamp). After a crash restart, while this node's last stamp is more than max_ahead
  past its wall, it counts as out of bounds: now() throws ClockOutOfBounds (up to about 25 s), so
  peers never meet a stamp they would refuse. A clean restart is unaffected.
