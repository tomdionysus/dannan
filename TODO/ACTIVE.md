# Dannan - Active Work

Last updated: 2026-11-14 (rationalised: history to `COMPLETED.md`). Dannan is written in this repository as its own
library, Macha's code at `00861dd` being the source of truth, and Macha is ported
onto it.
That reverses the morning's "in-tree first" (Tom, 2026-11-03: "I
misplanned. It should be here first."). "Resume here" below says what is
built. `COMPLETED.md` is the record; move items across as they land, with what
shipped and the gate counts.

The design is Tom's, 2026-11-02, in two documents: "Dannan: Distributed
Substrate Library Plan" (the library in full) and "Macha Core Extraction
Plan" (the seams, now read as port steps). Both are rendered here as
[`2026-11-02-dannan-plan.md`](2026-11-02-dannan-plan.md) and
[`2026-11-02-macha-core-extraction-plan.md`](2026-11-02-macha-core-extraction-plan.md);
these copies are the masters (Tom, 2026-11-04: the Claude Docs the
projects started from are starters only, to be deleted, never read for
direction or edited). Where the two differ the Dannan plan is the later
and wins. The Dannan plan's "Repository, build, versioning" now says
Dannan is its own repository from the start (Tom's decision above). The port of Macha is
[`2026-11-03-macha-extraction.md`](2026-11-03-macha-extraction.md): its
file table and each seam's "what must not change" and gate still hold,
its direction is reversed, and a note at its head says how to read it.
The list of everything Macha does that Dannan and Dagda must do, and that
Macha must still do once ported, is
[`2026-11-03-macha-function-inventory.md`](2026-11-03-macha-function-inventory.md).

**The sources, as of 2026-11-04.** `../macha` is at 0.92.2 (`5776392`),
cluster protocol 23, state layout v18, 785 test cases (744 + 20 + 21). The
live cluster is macnessa and inverbeg, both Pi 5 on Debian 13 with Boost
1.83. `../athenasip` is on `develop` at `8d65593`, tagged `0.12.0`; plugin
contract `API_VERSION` 24. The laptop is on MacPorts for everything. Macha's
current account of itself is `../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md`.

## Resume here (2026-11-14, 03:00Z)

**0.7.0 is released** (`main` at `6ce0349`, tag `0.7.0`, both pushed at Tom's word). Main's CI was green on every job. The
`.deb`s are in `../tomcully-apt/incoming/dannan/0.7.0/`; both library packages were checked: SONAME `libdannan.so.0.7`
and shlibs `libdannan0.7 (>= 0.7.0)`. **Tom still has to run** `sh publish.sh incoming/dannan/0.7.0/*.deb` and the
rsync; both are his steps, never upload. Dagda, Lir, Lugh and Macha Server were told. Lugh has 0.7.0 (P2 shows in its
Status with its P8.1). Lir is repinned with P2 wired in (`--minority degrade|refuse`), waiting on a Pi for its gate.

**0.8.0 is open**: the tree and the library's package say 0.8 (`e839322`). The approved batch, in order, each on its own
`feature/` branch, tests first, hosted CI, reviewed by the session that asked. Tom, 2026-11-14, to every session: no
test run is repeated without a flake or odd behaviour to justify it, and ASan and TSan are for releases (hosted CI's
jobs on push are the gate, not extra runs).
1. P6's remaining three, for Macha: **done** on `feature/owner-test-seam` and merged into `develop`. Macha reviewed
   it and every follow-up; all passed. CI run 43548808618 was green on every job. See `COMPLETED.md`.
2. P4: a class-aware worker pool with CPU pacing, from Dagda's `store/cpu_pacing.h`. Its proof is Macha's pacing cases
   (`test_repair_is_paced_not_stopped_while_higher_classes_stay_busy`, `maintenance_trace`) and a baseline run on the
   nodes.
3. P3: the shared public listener below the protocols, to Lugh's and Lir's combined requirements
   (`2026-11-13-stack-needs.md`, P3).
4. P5: small-message figures in `dannan-bench`, on the Pi cluster between two real machines. It covers notify fan-out,
   batched calls of 1 to 64 envelopes at 64 B to 1 KiB, msgs/s and p50/p99/p99.9, and the io thread count. Its stress
   case is the call path's ceiling, about 6,000 calls/s per peer pair, where calls are refused at
   `max_pending_replies` = 512 (`net/outbound.h:27`). Measure, then raise the cap, make it per class, or cheapen the
   per-call path. Dagda's bench reproduces it (`2026-11-13-stack-needs.md`, P5, has the build and the commands).
   Lir's target: PUBACK p99 under 50 ms at QoS 1 with k = 2.
P7, Alpine in CI: not now (Tom). The 0.6 and 0.7 work's detailed notes (each fix, each finding, each review) are in
`COMPLETED.md`, under "ACTIVE.md's history to 2026-11-14".

**Where things are built and tested** (Tom, 2026-11-14: "STOP all builds on the Mac now. Use the RPi cluster only."):
- Never build or run tests on the Mac. Editing, git, gh and reading CI logs are fine, and so is clang-format.
- Hosted CI (`.github/workflows/ci.yml`) is the gate: 15 jobs on Debian x86-64 and arm64 and macOS, with `abi`
  against the minor's last tag.
- To run CI on a feature branch, add a temporary commit putting the branch in `on.push.branches`. Drop it before the
  merge, with `git rebase --onto X^ X`.
- The shared Pi 5 cluster: corvus-jupiter 10.44.1.169, corvus-saturn 10.44.1.170, corvus-neptune 10.44.1.202. Debian
  13 arm64, 16 GB, NVMe, `ssh root@<ip>`.
  - Nobody schedules them (Tom): claim a free one first, first come, first served, then release it the moment the
    job ends.
  - From the next session start, Tom's claim-board plugin gives `claims_list`, `claim_take` and `claim_release`, and
    blocks heavy ssh commands without a claim. Before it: post `stack/pis/<host>/claims/Dannan` on
    http://10.44.1.169:8183 (`../lir/deploy/pi-cluster/README.md`).
  - One heavy job per Pi, a modest `-j`, nice 19: the Pis brown out under two.
  - Lir's standing cluster uses ports 7537, 1983 and 8183 there.
  - macnessa and inverbeg, which run Macha, still need Tom's word.

**Release rules unchanged:**
- Tags need Tom's direct word.
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

## Waiting on Tom

Each stops something; a session that has lost its context sees them
first.

Rulings on the first cutover, Tom, 2026-11-05, settled:

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
- **Disk limits are Dagda's; the generics are shared.** Tom, 2026-11-05: Dagda owns disk
  limits but should use the same work-class generics for them: "common code goes to the
  lowest level, Dagda depends on Dannan". Dagda already uses `WorkClass` and `WorkContext`.
  Under discussion with the Macha session: `sched::RetainedMemoryLedger`, Dagda's
  `DataResourceArbiter` and its `IoReserve` look like one admission mechanism written three
  times, with different policies. That Dagda is mid-work does not hold this back; Dagda
  takes direction from this session.
- **Live nodes.** The Macha session's own loopback runs on macnessa were fine. The hold
  covers Dannan's work, the Pi measurements and the gateway.
- **The macnessa copy.** Allowed, read-only, without stopping the node: the roster, inbound
  and telemetry files only (step 7).
- **The Pi baselines.** The Macha session's, later, at his word, with the deployed version.

Held until he says: the Pi baselines and measurements, the real IGD run, anything on
macnessa or inverbeg. Each push and release is his word too.

0. Tom, 2026-11-04 (after 0.3.0): the Macha port (steps 3, 6, 8) **not
   yet**; the AthenaSIP port (step 11) **not yet**, until Dagda's heads and
   leases have a tagged release. Meanwhile Dannan serves Dagda, Lugh and
   Lir. Pushed at Tom's word the same day: `origin` is
   `git@github.com:tomdionysus/dannan.git`, holding main, develop and the
   tags 0.1.0, 0.2.0, 0.3.0 (0.4.0 pushed at his word too). Each later
   push still needs his ask.

1. **The suite counts on the live nodes** at `0.91.2`, and at each Macha release
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
7. Settled (2026-11-04): the repository copies are the masters, so the
   design's "starts in-tree" paragraph was corrected here to Tom's
   decision that Dannan is its own repository.
8. Settled (Tom, 2026-11-03): core message types are Dannan's; the rest
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
  on Macha development does not cover it; deployment to the live nodes is the gate
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
- (2026-11-04, Tom, asked in turn) **The stack's open questions, settled:**
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
- (2026-11-04, Tom, asked in turn) **Identity (step 10)**: certificates
  are X.509 through OpenSSL; they are wire version 2 beside version 1 (a
  node with both speaks both, so a cluster moves node by node); the CA key
  is offline, used through a `dannan-ca` tool, never on a node. The design
  is `TODO/2026-11-04-identity.md`.
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
  `../macha/TODO/ACTIVE.md`, and deploys to the live nodes when convenient.
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
- **Nothing is published.** The design is in this folder, and these
  copies are the masters (Tom, 2026-11-04). Pushing is a separate,
  explicit request.
- Code lookups in Macha and AthenaSIP go through basemind when it is up
  (all three trees carry `basemind.toml`); rescan after edits.
- The Macha session (another Claude session on this machine, "Macha
  Server") works in `../macha` on Tom's Macha asks; tell it before a port
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
