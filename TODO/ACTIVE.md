# Dannan - Active Work

Last updated: 2026-11-03, evening. Dannan and Dagda are written in this
repository, from the design, as their own libraries, and Macha is ported
onto them piece by piece. That reverses the morning's "in-tree first"
(Tom, 2026-11-03: "I misplanned. It should be here first."). Nothing is
built yet: the repository has had `git init` and holds no commits.
`COMPLETED.md` is the record; move items across as they land, with what
shipped and the gate counts.

The design is Tom's, 2026-11-02, in two documents: "Dannan: Distributed
Substrate Library Plan" (the library in full) and "Macha Core Extraction
Plan" (the seams, now read as port steps). Both are rendered here as
[`2026-11-02-dannan-plan.md`](2026-11-02-dannan-plan.md) and
[`2026-11-02-macha-core-extraction-plan.md`](2026-11-02-macha-core-extraction-plan.md);
the Claude Docs are the masters and the renderings are re-made when they
change. Where the two differ the Dannan plan is the later and wins. One
paragraph of the Dannan plan ("Repository, build, versioning": "Dannan
starts in-tree in Macha") is superseded by Tom's decision above and
waits on his edit of the doc (Waiting on Tom, 7). The port of Macha is
[`2026-11-03-macha-extraction.md`](2026-11-03-macha-extraction.md): its
file table and each seam's "what must not change" and gate still hold,
its direction is reversed, and a note at its head says how to read it.
The list of everything Macha does that Dannan and Dagda must do, and that
Macha must still do once ported, is
[`2026-11-03-macha-function-inventory.md`](2026-11-03-macha-function-inventory.md).

**The sources, as of 2026-11-03, evening.** `../macha`'s `develop` is at
`a547ddf`: `0.91.2` (`6c7ec64`, tagged, the baseline) plus the transport
proofs (`c71370e`), the dropped-replies fix (`d61eb02`, "0.91.3") and the
example and documentation fix (`353e600`, "0.91.4"); neither release is
tagged or pushed. The checkout is on `develop`, clean, `develop` and
`main` its only branches: Tom had the Macha session clean up the
morning's leftovers (the goldens branch, the two-process script, the
0.91.2 worktree), so the port gate's config and Status goldens are
written afresh at step 3; the deleted commits `2dfbb02` and `6a797af`
stay in Macha's reflog for about ninety days should their text be
wanted. Cluster protocol 23
(`src/cluster/net.cpp:96` now), state layout v18. Laptop gate: 682 + 19 +
21 on `develop`; the fi-1 counts are unrecorded. One live node (fi-1); gbni-1 is out since
2026-10-13; Macha's own development is paused except what Tom asks for
(today: 0.91.3, the example and the docs, by the Macha session).
`../athenasip` is on `develop` at `8d65593`, tagged `0.12.0`, clean;
plugin contract `API_VERSION` 24; two live nodes, neither production. The
laptop is on MacPorts for everything (Boost 1.88 the only Boost; the
configure line for Macha is in `COMPLETED.md`).

## Resume here (2026-11-03, night)

Written for a session starting from nothing. Steps 0, 1 and 2 are done
and released as `0.1.0` (`main`, tag `0.1.0`, not pushed; `COMPLETED.md`
has every milestone with its gate). `develop` holds the same tree;
`feature/transport` is merged. Gate: `dannan_tests` 206, `dagda_tests`
1, `dannan_interop_tests` 8 (the configure line is under "How to prove
it").

Steps 4 and 5 are done: the control plane, and the node with
`dannan::Context`, closed against Macha's every use of its network layer
(`TODO/2026-11-03-macha-on-context-audit.md`). Wire v1 is in code on
`feature/wire-v1` (gate: `dannan_tests` 315, `dagda_tests` 1; the Macha
interop suite is retired with protocol 23, Decisions): the version range,
Dannan's labels and magics, the node record v1 with capabilities, path
class, candidates and the advert map (`Nodes::advertise`), the renumbered
core types with user types from 1024, the observed address in the ack and
the path token inside the hello. Level 0 is in code too (gate 323 + 1):
path classes and candidates, the brokered punch, the relay
(`reach/paths.*`). The Docker compose test bed is written
(`test/compose/`: two public relaying hubs, four NATed nodes, two of them
punchable, one behind a randomising NAT, one behind two NATs; the runner
`test/compose/scenario.py` checks classes, paths, echoes on both lanes, a
healed partition and a relay failover) and passes (`COMPLETED.md`).
Next: the stack's foundations (below, "Foundations for the stack":
secure memory, signing, Argon2id, node key custody, channels and the
roster feed, the node's custody and fuzz targets done). Section 6's rules are all in code (moving up a class, the relay's DATA
budget, placement told a peer's path class). Identity (step 10) waits; the port steps wait
for the Macha agent and Dagda is another project (Decisions, 2026-11-03
night).

Read in this order: this file to the end of Standing rules;
`2026-11-02-dannan-plan.md` whole; the inventory; the head of
`2026-11-03-macha-extraction.md` and its file table;
`../macha/docs/principles-and-laws.md`; then, for the step in hand, the
Macha files the inventory names for its rows.

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

## Waiting on Tom

Each stops something; a session that has lost its context sees them
first.

1. **The suite counts on fi-1** at `0.91.2`, and at each Macha release
   the port makes, recorded in `COMPLETED.md`. A deploy-day act under
   Macha's rules.
2. **Cloud providers and budget for the chaos cluster**, when level 0 is
   ready to be tried for real. Until then: the compose cluster only (Tom,
   2026-11-03), and no cloud spend.
3. **Macha findings that are Macha's to settle**, noted here only:
   `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`
   is flaky on macOS at the baseline (1 run in 5), and
   `maintenance_trace/test_trace_backend_offline_and_back` on macnessa
   (Macha session, 1 in 3). Both are with the Macha session.
4. Settled: the first commit is `2a923f5` on `main`; `develop` branched.
   `CLAUDE.md` stays uncommitted (Tom's global gitignore excludes it).
   Tom (2026-11-03): commit at each completed milestone without asking;
   no attribution lines, ever.
5. Settled: Tom had the Macha session delete the morning's leftovers in
   `../macha` (the goldens branch, the two-process script, the 0.91.2
   worktree and its runs). The goldens are rewritten at step 3 as part of
   the port gate; the two-process test is written here at step 2.
6. Settled: GoogleTest 1.18.0 from MacPorts (`gtest`), the only one on
   the laptop.
7. **The design doc's "starts in-tree" paragraph** (Repository, build,
   versioning), which Tom's decision supersedes: his edit of the Claude
   Doc, after which the rendering is re-made.
8. Settled (Tom, 2026-11-03): core message types are Dannan's; the rest
   are user-definable and under the user's control. See Decisions.

9. **Revoking a single node at once** (Lugh, 2026-11-03 night): Lugh needs
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
the laptop and then on fi-1: the nine items of "The seam gate" in the
extraction document, which are: the three suites with counts equal or
above, the config golden, the Status golden, the route list, the plugins,
the install, wire and disk (protocol 23, layout v18, the mixed-build
two-process test), the diff reviewed with every content change named,
and the inventory updated. Macha's rules on the laptop hold: no verdict on
timing under load, touched tests here, all three suites on fi-1, never
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
- (2026-11-02, design; **superseded 2026-11-03 evening, Tom**) ~~In-tree
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
- (2026-11-03, Tom) **Status is a Dannan noun.** One Status surface per
  node: `publish(key, value)` and `counter(name)` on the context, every
  hosted system publishing into it, the parked state and the counters of
  Dannan itself beside them. Macha's `GET /api/v1/status` reads Dannan's
  surface plus its own and stays byte-identical (the Status golden);
  AthenaSIP's `GET /api/v1/nodes` reads the same surface. The telemetry
  record's generic shape at protocol 25 is this surface's gossip.
- (2026-11-03, Tom) **The laptop is MacPorts for everything.** Done.
- (2026-11-03, Tom) **Extraction proceeds on the laptop now.** The pause
  on Macha development does not cover it; deployment to fi-1 is the gate
  it already is, taken when convenient.
- (2026-11-03, Tom) **The store is Dagda.** Target `dagda`, directory
  `dagda/`, namespace `dagda`. The Dagda's cauldron never runs empty.
- (2026-11-03, Tom; applied from the first file, not at a split)
  **AthenaSIP's mechanics are Dannan's style**: Google base, 160 columns,
  two-space indent, `.h`, the header block, `_name` members,
  `dannan::<subsystem>` namespaces. Macha itself stays in its own style.
- (2026-11-03, Tom) **Boost.Asio from the start.** The transport is
  Dannan's first substantial piece (step 2), on Asio, speaking protocol
  23 byte for byte, with every transport behaviour in the inventory kept
  and the executor rule native. Macha replaces `cluster/net.cpp` with it
  at step 3, before anything else of Dannan exists, because the thread
  model is what the single live node exercises hardest.
- (2026-11-03, Tom) **Head spaces carry a replica policy from the start.**
  `replicas: N` is accepted at `open()` in v1, with CONTROL placement per
  space and forwarded reads over CONTROL that say so in the evidence.
  Macha's own space is opened every-node.
- (2026-11-03, Tom) **Scope is looked up in the Lugh head at write time**,
  by every replica that counts toward the floor; a node that has lost the
  Lugh space accepts a short-lived credential's scope instead.
- (2026-11-03, Tom) **TCP simultaneous open first; UDP later.** Level 0
  ships with TCP punching and relay; the lanes, `SecureChannel`, fragments
  and BBR are untouched. A UDP lane is a later step.
- (2026-11-03, Tom) **Two protocol bumps**: 24 for level 0 (path classes,
  relay, the punch broker's messages), 25 for identity (certificates in
  the handshake). Level 0 is proven on a cluster before the handshake
  changes.
- (2026-11-03, Tom) **The compose cluster only, for now.** One machine,
  docker networks for NAT, a scenario runner for terminate, partition and
  clock skew. No cloud spend until level 0 is ready to be tried for real.
- (2026-11-03, Tom) **Write every owed transport proof before the
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
- (2026-11-03 night, Tom) **Macha is the first customer. The design is not
  complete until Macha can be refactored to use it.** Every call Macha
  makes into its network layer must map to a `dannan::Context` operation
  before step 5 counts as done; the audit is
  `TODO/2026-11-03-macha-on-context-audit.md`, and its gaps are work, not
  questions. Macha's move to the new wire is a flag day (no protocol 23
  mode), noted for the Macha agent.
- (2026-11-03 night, Tom) **Dannan is a new project with its own wire,
  versioned from 1.** It is not an extension of Macha; Macha is refactored
  to use it. This supersedes "the one rule"'s byte-for-byte protocol 23 and
  "No byte moves": nothing of protocol 23 binds Dannan (versions, labels,
  magics, record layouts, message numbers). Dannan does everything to
  maintain the PHY (reachability included). Level 0's wire is
  `TODO/2026-11-03-dannan-wire-v1.md`. Docker is used for the compose test
  bed and nothing else. Identity (step 10) waits.
- (2026-11-03 night, Tom: "Continue please" on the draft) **Wire v1 as
  drafted, with the draft's recommendations.** The Macha interop suite
  (`dannan_interop_tests`), `tools/macha_types.h` and `dannan-node`'s Macha
  script go with protocol 23; "a test is never deleted" is set aside for
  them by this decision, and a Dannan-only two-process test replaces the
  cross-process proof. A punched or relayed connection proves its token
  inside the hello (a purpose byte and a 16-byte token), so a stray
  connection fails the handshake.
- (2026-11-03 night, Tom) **Dagda is its own project, with its own agent.**
  Steps 7 and 8 leave this plan; the `dagda/` stub here goes when that
  project starts. **Macha is not moving yet:** the port steps (3, 6) are
  the main Macha agent's, later. This plan finishes Dannan's own code:
  steps 4 and 5, then Dannan's part of 9 and 10. Dannan's API is not
  stable and nothing uses it yet: change it freely.
- (2026-11-03 night, Tom) **Core message types, and user types under the
  user's control.** Dannan's own types (`with_dannan_types`) are core:
  their rules are Dannan's and cannot be redefined. Every other number is
  a user type: its lanes, classes, notification flag and handling are the
  registrant's. A user type nobody on this node registered is not
  policed (either lane, any class) and goes to the user's handler, a
  notification included, so a peer is never cut off for traffic this node
  does not host. Supersedes protocol 23's default (control lane, control
  frame only) for unknown numbers; no byte changes. Follows from it, Tom's
  question the same night: a handler reports whether it processed the
  message, failed, or does not recognise it.
- (2026-11-03 night, Tom) **One stack, no duplicated code.** Dagda and
  Lugh use Dannan as their network layer; Lugh uses Dagda as its
  datastore. No code is duplicated between the three projects: what more
  than one needs (crypto primitives, durable files, the thread-safety
  contract, time sources, logging, the Status surface) lives once, in the
  lowest project that needs it, and the others link it.
- (2026-11-03 night, Tom) **Identical styles, conventions and layouts** in
  all three: one `.clang-format`, one header block form, one naming scheme,
  one repository layout (`<project>/<subsystem>/`, `<project>/include/`,
  `<project>/test/`, `tools/`, `docs/`, `TODO/`), one branch and tag
  scheme, one set of rules in `CLAUDE.md` and `CONTRIBUTING.md`.
- (2026-11-03 night, Tom, in Lugh's session, as Lugh reported it) **Lugh
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
  Tom are there, not restated here.
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
  `../macha/TODO/ACTIVE.md`, and deploys to fi-1 when convenient.
- **Report findings before fixing; prove a mechanism before editing.**
  Design questions go to Tom with a recommendation. Decisions above are
  not reopened without asking.
- **A port is reviewed as a swap**: the Macha diff removes a file or a
  class and adds a registration or an adapter; a content change beyond
  that is named in its commit, and a port whose diff touches a codec is
  wrong.
- **Every step ends with the inventory updated**: each row it covered
  gains the Dannan or Dagda case that proves it here; each row a port
  touched keeps its Macha case as the port's gate.
- **Nothing is published.** The design docs are Tom's Claude Docs; this
  folder holds renderings. Pushing is a separate, explicit request, and so
  is a commit.
- Code lookups in Macha and AthenaSIP go through basemind when it is up
  (all three trees carry `basemind.toml`); rescan after edits.
- The Macha session (another Claude session on this machine, "Macha
  Server") works in `../macha` on Tom's Macha asks; tell it before a port
  session touches that tree, and it tells this one.

---

## Next

In order. A build step and the port step that follows it alternate, so
the boundary is pulled on from Macha's side as early as possible. Nothing
here waits on a device, a carrier or another repository, except the items
under Waiting on Tom.

### 0. This repository (2026-11-03)

- [x] `TODO/`: the plan, the record, the port proposal, the function
      inventory, the two design renderings.
- [x] The design questions put to Tom and answered (Decisions).
- [x] `CLAUDE.md`, `CONTRIBUTING.md`, `.clang-format`, `basemind.toml`,
      `.gitignore`; a "Dannan extraction" section in Macha's
      `TODO/ACTIVE.md`.
- [x] The first commit on `main` (`2a923f5`); `develop` branched.
- [x] The build skeleton (2026-11-03): `CMakeLists.txt` with `project(dannan VERSION
      0.1.0)`, two shared libraries `dannan` and `dagda`, GoogleTest
      binaries `dannan_tests` and `dagda_tests` behind
      `-DDANNAN_BUILD_TESTING=ON`, `-DDANNAN_WARNINGS_AS_ERRORS=ON`, Boost
      (1.87 or later, Asio) and OpenSSL found from `/opt/local`, clang
      `-Wthread-safety`; the directory layout of the design (`dannan/` with
      `include/dannan/`, `reach/`, `net/`, `membership/`, `placement/`,
      `sched/`, `supervise/`, `identity/`, `status/`, `contract/`, `test/`;
      `dagda/` with `domain/`, `store/`, `ledger/`, `heads/`, `lease/`,
      `contract/`, `test/`); `--version` from a tiny `dannan-version`
      executable or a test, agreeing with the tag.
- [x] `docs/principles-and-laws.md`: a verbatim copy of Macha's, with one
      provenance line; cited by number from here on.
- [x] The first test fixtures under `dannan/test/fixtures/`: the gate and
      the free-port allocator (2026-11-03). Arriving with the steps that
      give them something to stand on: the stepped time source (step 1,
      with `TimeSource`), the in-process cluster of Dannan nodes (step 2,
      with the transport), forked-process isolation where a case needs it.

### 1. The vocabulary (step 1) - done 2026-11-03

What the transport stands on, each small and specified: `sched/` (the
work classes `control`, `foreground`, `read_ahead`, `loader`,
`speculative` and `frame_for`; declared waits and the wait guard; the
retained-memory ledger with its three reserves and the reassembly reserve
inside one total, shrunk pro rata under a memory limit with one warning;
budgets, cursors and pages; the retry policy), `contract/` (thread safety,
`TimeSource` with a stepped source for tests, published snapshots), the
codec (Writer and Reader, byte-compatible with Macha's wire encoding of
integers, bytes, strings and the node record), the scoped logger, and
`durable_file`. Tests: the inventory's "Work classes, waits and budgets"
table whole and the time and durable-file rows of "Identity, time, state
files". Gate: `dannan_tests` green, every row named.

### 2. The transport on Asio (step 2)

`dannan::net`, the first substantial piece, from the design's Level 1 and
the inventory's transport table, speaking protocol 23 byte for byte:
`SecureChannel` (the v15 handshake with the cluster key, X25519 and HKDF,
AEAD per fragment with counters, the session id, frame size negotiated
and bounded, the lane byte, shutdown and close under one mutex); the two
lanes and the probe lane; the open route table (16-bit type, registered
with lane, class, notification flag and handler; Dannan's reserved
numbers from the extraction document's seam 4; collisions refused); the
client (dial on demand and reuse, shared cold dials, simultaneous-dial
reconciliation, the outbound limits with the small-message reserve, the
pending-reply budget with the refusal's reason, stall notice and
no-progress deadline per lane, cancel, abort and promotion on both sides,
the health loop with per-peer backoff and the smoothed round trip from
pings only, the three broadcasts, reverse dial and maintained peers for
inbound-incapable nodes, the probe dial, traffic totals in one shared
instance); the server (accept, the pre-auth bound, session reaping with a
session count in its work stats, request classes with byte-bounded queues,
the fast-control allow-list, the per-peer-ordered bounded executor that
Macha's metadata mutations need, rejection counters, per-type and
per-frame timings, 0.91.3's reply rule); the executor rule native (every
completion posts to a caller-supplied `any_io_executor`; the blocking
call is a wait on it with its declared waits); `RpcLinks` as the seam
between the client and its sockets so tests inject faults. Open design
points to settle at the start of the step, with Tom if they change the
design: how the per-peer-ordered executor is expressed (a property of the
registration, recommended) and which of the server's queue limits are
Dannan's per-class budgets.

Gate: the transport table, every row, as `dannan_tests` cases (the 21
Macha cases of `c71370e` and the earlier ones are the model); the
two-process test against the Macha binary (`build-macports/macha` at
Macha's `develop`), both ways round: handshake, the Dannan node listed in
Macha's Status, `ping` and `members`, telemetry received, a DATA-lane
call; a frame capture of one session compared byte for byte with Macha's
own. Then tag `0.1.0`.

### 3. Macha on Dannan's transport (port step)

In `../macha`, on `feature/dannan-port-transport`: first the port gate's
two goldens, written afresh (the config dump, field by field with
compile-time member counts, and the Status surface from two nodes in
process, as the deleted `2dfbb02` and `6a797af` did; their text is in
Macha's reflog if wanted), committed before the port so they measure it.
Then Macha links `dannan` at `0.1.0`; `cluster/net.{hpp,cpp}`, `message_routes.hpp`,
`frame_type.hpp` and `rpc_refusal.hpp` are replaced by Dannan's, with a
thin adapter where `NodeRuntime` expects Macha's names; Macha registers
its message numbers (the extraction document's seam 4 lists Dannan's,
Dagda's and Macha's); `Config` and `NodeRuntime` are untouched beyond
what the adapter needs. `CONTRIBUTING.md`'s line that Boost is not
required there goes. Gate: the port gate whole; `test_rpc_cluster.cpp`
every case; the two-process mixed-build test (0.91.2 or 0.91.3 binary
against the ported binary); the Status golden (RPC and work stats are
fields of it); a deploy to fi-1. Macha release `0.92.0`.

### 4. The control plane (step 4, in sessions per table)

- 4a `membership/` and `placement/`: the gossiped roster with the
  grandfathered `NodeInfo` record, direct liveness and `dead_after`,
  forgetting at the horizon, persistence under the state path, identity
  association resets, flags as a capability set reading the same byte,
  `all_known_reachable`, the write-floor advertisement; rendezvous and
  capacity-aware placement, failure domains, commit-replica ordering.
  Tests: the inventory's "Membership" and "Placement" tables.
- 4b `identity/` v1 (the cluster key file, the durable node id, the
  durability epoch, the state-path lock), `reach/` v0 (`inbound_capable`
  resolution by dial-back, persisted, re-probed by cadence; UPnP, external
  IP and the self-probe as `PublicConnectivity`), `status/` (telemetry
  gossip with the grandfathered record, activity clocks, node events, the
  Status surface with `publish` and `counter`, observation windows).
  Tests: "Public connectivity", "Telemetry and status", the rest of
  "Identity, time, state files".
- 4c `supervise/`: the supervisor, plugins discovered from a directory,
  the build-identity check, decline, fault and rebuild, the registry by
  name, startup progress and its no-progress exit, the lifecycle stop
  token. Tests: the "Supervision" table.

### 5. The node and `dannan::Context` (step 5)

Dannan's node: the control plane assembled (what the extraction document's
seam 2 lists for the first class), the control routes it binds, the
membership, connectivity and telemetry loops, `refuse_impossible_cluster`,
readiness, stop and cancellation; `dannan::Context` with the design's
nouns (Nodes, Routes, Channels v1 as notify on CONTROL and bulk on DATA,
Identity v1, Budget, Faults, Time, Status), written as a spec a second
implementation could satisfy. Gate: a Dannan-only two-node in-process
cluster; a Dannan node joining a running Macha cluster as a bare member
(the two-process test grows); a hosted system that is nothing but a
counter runs on it.

### 6. Macha on Dannan's node (port step)

In `../macha`: the config split (seam 1 of the extraction document, now
`dannan::Config` read by Dannan and the rest Macha's) and the
`NodeRuntime` split (seam 2: Dannan's node under Macha's facade, which
keeps the advertisements, the generation announcements, `hosts_extents`
resolution and the playback budgets); `subsystem/*`, `contract/work`,
`retained_memory`, `time_source`, `membership`, `placement`,
`node_identity`, `public_connectivity`, `telemetry`, `activity_clocks`,
`node_events` replaced by Dannan's; `SubsystemRegistry` publishes by
name; the FUSE and torrent plugins load through Dannan's supervisor. Gate:
the port gate whole, the config golden and Status golden byte-identical,
the plugin cases, fi-1. Macha release `0.93.0`.

### 7. Dagda (step 7, in sessions per table)

From the design's store sections and the inventory's store table, byte
compatible with layout v18 and protocol 23: `domain/` v1 (one domain,
every node a member; the blind role specified, not yet exercised);
`store/` (local stores, packs, pool, durability domains and tickets,
I/O pressure, the persistent cache, retention); `ledger/` (the trie, the
diff, the union, the held and retention ledgers, reference counts, the
horizon builder taking a `ReferencedSets` provider, the three destructive
gates, GC's two sweeps); `heads/` (the opaque commit DAG: commits,
acceptance at the write floor with certificates, head sets, per-origin
dots, compaction, the Merkle tree over keys, the reconciler calling the
owner's merge, conflicts as durable records, `replicas: N` per space
with CONTROL placement and forwarded reads; the grandfathered FS codec as
a codec the FS supplies); `lease/` (the lease and the claimed collection
templates, `TorrentRequest`'s rules as the first instantiation with
identical bytes); `dagda::Context` (Domains, Objects, Claims, Heads,
Leases, Tickets, Retained). Gate: `dagda_tests` from the inventory and
`VALIDATION.md`'s gates; the three gates the extraction plan names
(divergent heads reconcile with no lost commit; repair by diff converges
after a partition heals; a torn volume rejoins without hand repair) in
process; the two-process test with objects on both lanes, commits both
ways and a divergence merged against the Macha binary.

### 8. Macha on Dagda (port step, the snapshot last)

In `../macha`, in the extraction document's order for seams 6 to 9: the
reachability provider (the catalogue supplies its sets; the ledger stops
including it), the lease template (`TorrentRequest` through it, bytes
identical), opaque heads (`MetadataSnapshot` divided into Dagda's head
record and the FS state blob under the grandfathered codec;
`metadata_merge.cpp` to `filesystem/` as the FS merge; `MetadataView` as
Macha's facade), then the FS through the two contexts alone
(`SubsystemContext` stops handing out `filesystem`, `hydration`, `ingest`
and `local_state`). Gate: the port gate with the mixed-build test
(baseline binary against the ported one: a cluster, commits both ways, a
divergence merged, the result identical from either side);
`macha-metadata-dump` identical on a fixture state directory; fi-1 for
the period the extraction plan names. Macha release `0.94.0`. After this
the door exists, and the second hosted system comes through it and
nothing else.

### 9. Level 0: reachability (step 9)

First the compose cluster (Tom: compose only): one machine, docker
networks for NAT, a scenario runner for terminate, partition and clock
skew, the port gate's checks runnable on it. Then `reach/` in full:
observed-address learning from every inbound frame, confirmed only when
another node has reached them; path classes tried in order per pair; the
punch broker over CONTROL with TCP simultaneous open; relay by a node
with the `relay` capability, CONTROL relay unconditional, DATA relay
charged to the relaying node's loader class and published in Status; a
path change as a reconnect under the same identity. Protocol 24, the
first bump, rolling. Proven by making half the compose cluster relay-only
and asserting the same gates; then on the chaos cluster when there is one
(Waiting on Tom, 2).

### Foundations for the stack (2026-11-03 night, under "no duplicated code")

Dannan is the one home of what Dannan, Dagda and Lugh all need. Written
here from Dannan's design (Lugh's code read for what it does, never pasted),
each announced to the Lugh and Dagda sessions as it lands, so they delete
their copies:

1. Secure memory: key material locked in RAM, kept out of core dumps and
   forks, wiped on release; a `Secret` that cannot be logged.
2. Signing: EC P-256 (ES256) and Ed25519, sign and verify, PKCS#8 keys.
   Step 10's CA uses them too.
3. Node key custody, an early slice of step 10: the node's private key
   and the root Dagda seals node-local keys under, loaded from a sealed
   systemd credential or a typed passphrase, held in secure memory.
4. Fuzz targets for every parser of untrusted bytes (`dannan/fuzz/`):
   handshake, frame header, assembler, node record, telemetry, the level 0
   payloads.

5. Channels with named delivery contracts (plan, "Level 1"): notify
   (CONTROL, at most once, dropped under pressure), stream (CONTROL, at
   least once, a per-origin sequence, bounded replay from the sender),
   bulk (DATA, backpressured, acknowledged); subscription node-local by
   topic prefix, gossiped as an advert, so a publish goes only to nodes
   with a subscriber. Retained is Dagda's (a notify whose last value lands
   in a head). Lir's lir.notify, lir.seq and lir.bulk sit on these.
6. A membership-change feed (joined, left, changed) with each node's
   incarnation, the durability epoch carried in the node record.
7. The node opening its custody (`NodeConfig`), its node key and derived
   keys reachable through the Context's Identity.

Lugh keeps what only Lugh needs: HMAC-SHA1, base32, base64url, JOSE.
Argon2id moved here: custody stretches a passphrase with it too.

### 10. Identity (step 10)

Requirements from Lugh (2026-11-03 night), part of the step: a single
node revoked and refused at once (proposed: a CA-signed, grow-only
revocation record, gossiped and persisted like an identity reset, refused
at every handshake and its sessions closed); and a node's private key held
as Lugh holds its master key (a TPM-sealed systemd credential where there
is one, locked memory, never on disk in clear), since Dagda wraps domain
keys, Lugh's among them, to node certificates. And the scope seam:
`contract::ScopeSource` declared here, `Identity::verify` over it, Lugh
installing the source (the stack document).

AthenaSIP's `cluster_ca.cpp` written into `identity/` in Dannan's style;
Macha's shared cluster key becomes a one-class CA; `SecureChannel` keys
from the certificate; the node id is the certificate's. Protocol 25, the
second bump, rolling, and the moment the grandfathered wire records
(`NodeInfo`, telemetry) gain their generic advertisement maps and the
capability set leaves the flags byte. Scope looked up in the Lugh head at
write time with a short-lived credential as the fallback.

### 11. The AthenaSIP port (step 11)

`Datastore` and `EventSystem` implemented as Dannan-backed drivers behind
the existing plugin interfaces, the SIP stack untouched; heads for its
records, notify channels for its signals, leases for trunk registration,
presence takeover and the orphan-call sweep, per-origin counters for
`counter_add`, retained status from gossip, `AddressDiscovery` retired
into level 0. Redis and Mosquitto become optional, then go. Gate:
`test/e2e/cluster.sh` with the second node behind CGNAT and no public
address. The extraction document's "What AthenaSIP needs" has the
mapping.

### 12. The six (step 12)

Event system, KV store, git forge, compute, mail, general filesystem, in
that order. Any core change one of them needs is a step proven on Macha
and AthenaSIP first. Not planned here.

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
