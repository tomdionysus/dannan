# Dannan - Active Work

Last updated: 2026-11-24, after 0.12.1's release. `COMPLETED.md` is the record; move items across as
they land, with what shipped and the gate counts. The Cluster Stack's items are numbered as in
[`2026-11-20-stack-plan.md`](2026-11-20-stack-plan.md); the library's design is the maintainer's,
2026-11-02, in [`2026-11-02-dannan-plan.md`](2026-11-02-dannan-plan.md). These copies are the
masters (the maintainer, 2026-11-04); the Claude Docs the projects started from are never read for
direction or edited.

## Resume here (2026-11-24)

**0.12.1 is the last release** (2026-11-24, tag on main's merge 9dba83b): a patch on 0.12.0's ABI,
the call watchdog's fix. **0.12.0** (2026-11-24, tag on b9a5609) shipped item 1 below, four days
ahead of its target; its arm64 package build failed on the watchdog race, so 0.12.0 has x86-64
packages only and 0.12.1 has both. Both are staged for the apt repository (0.11.1 and earlier are
on it, checked against the live Packages files). Consumers move their pins from 0.11.1 or 0.12.0
to 0.12.1, which changes nothing else; Mimir pins 0.10.1 until its 0.2.0. AthenaSIP is a direct
consumer (the maintainer, 2026-11-23: a dagda:// datastore and a dannan:// event system, one
Dannan per node).

**Next:** the register layer (2), first in 0.13.0, then atomic commit's code the day it is on
develop (the maintainer's reprioritisation, 2026-11-24, below); the backfill's first part
alongside.

Shipped since the stack plan, so no longer to do: items 1, 6, 9, 12, 13, 15, 16, 17 (the fuzz
export, the stream carrier), 19, 28, 24's JSON half, the shared node settings under cluster.*, the
transfer-buffer pool, in-place AES-GCM, liveness_time, SteppedTime(poll) (0.10.2), the TSan
suppressions, Delivery::ordered (0.11.1).

**The order** (the coordinator's survey, 2026-11-23, of who waits on what; most-waited first,
then what the next one builds on). Each minor changes the ABI; within one, only additions.

1. **0.12.0, shipped 2026-11-24** (target was 2026-11-28; COMPLETED.md), with 0.12.1 after it:
   - Readiness and health (7): Lugh, Louhi, Macha, Kvasir, Dagda's store health; HTTP's health
     route and the node-program helper's drain build on it, so it goes first.
   - Keyed limits (10) and the Argon2id credential (11): Lugh, Mimir, Kvasir, Macha, Louhi's
     password sign-in. Small and independent.
   - The node-program helper (14): Lugh P8.1, Mimir, Kvasir, Louhi; over the node settings and 7.
   - The process runner (23): Louhi, Macha; from Louhi's command.h.
   - Small, riding with it: the notifying TimeSource (Dagda's TSan stalls); deferred requests
     bounded in bytes per peer (Macha); `WallClock::unix_us()` with a default of unix_ms() * 1000
     and a stepped wall clock in microseconds (Kvasir's NOW(6)); a `prepare(i, node)` hook in
     SimNodes::start before each node starts, so a hosted system registers its routes (Mimir,
     2026-11-23); the hybrid clock's ceiling written only for stamps issued (the approved 0.9.3
     fix, never shipped: `keep_ahead()` still follows the wall, hlc.cpp:159).
2. **0.13.0, target 2026-12-05** (with Manannan's four small asks, 2026-11-23, relayed by the
   coordinating session; Manannan's 0.5.0 waits on 0.13.0's HTTP server anyway, and depends on
   nothing it does not ask for here). **The register layer goes first** (the maintainer's
   reprioritisation, 2026-11-24, from the coordinator's critical-path pass): it heads the longest
   chain in the group (the register layer, atomic commit in 0.14.0, Mimir's multi-key writes,
   AthenaSIP's Mimir scripts; Kvasir's cluster, its step 4, needs atomic commit too). Its shape
   fixed, atomic commit's design goes to Mimir and Kvasir at once, and atomic commit's code starts
   on its own branch the day the register layer is on develop, before 0.13.0 is tagged. The HTTP
   core follows within 0.13.0: it unblocks four dashboards and Manannan's API, none of which chain
   further. Then the DNS client, slow peers, Manannan's asks and the watchdog seam.
   - The register layer (2), first: Lugh P8.6, Mimir, Louhi, Kvasir.
   - Named-slot placement: a pure function filling N named slots (ns1 to ns4) from a candidate
     set, distinct nodes, each slot moving only when its own node leaves the set; recomputed on
     Nodes::watch.
   - A true-time interval for signing (RRSIG windows): the wall time with the bound the node's
     clock is held to, empty while the node is out of bounds.
   - Revocations watched: Identity gains a watch that tells each revocation applied here.
   - The external address per family: IPv4 and IPv6 each with when it was observed, beside the
     one external address PublicConnectivityStatus has now.
   - The HTTP core (24): Lir, Louhi, Kvasir and Mimir's dashboards, and Lugh; the health route
     behind 7.
   - A ClientOptions seam called after a call is handed to its session and before its watchdog
     arms, and the test 0.12.1's fix waited for: the clock moved exactly in that gap, the first
     notice still on time (a test that fails every run on 0.12.0's watchdog).
   - A DNS client (the maintainer, 2026-11-23), beside the HTTP client, which needs it: asynchronous
     on the caller's executor with a deadline (getaddrinfo blocks a thread), A, AAAA, SRV and TXT,
     its answers cached by TTL, servers from the system's resolv.conf or named, over UDP with TCP
     on truncation. It works with any DNS servers and never relies on Manannan. Every project on
     Dannan gets it, and Dannan's own name lookups move onto it: seeds named by host, the stream
     connector's dials, and the HTTP client. The shape goes to the consumers first. Their answers
     (2026-11-23):
     - Lir: none of its own; the connector's dials must look up within the dial's own deadline,
       and try a name's several A and AAAA answers within it, interleaved as RFC 8305 does, so a
       dead first address does not fail the attempt. resolv.conf's servers are enough.
     - Kvasir: a reverse lookup (PTR) and its forward confirmation, for MySQL's host-named
       accounts; a lookup that failed or timed out told apart from "no such name" (not_found
       against unavailable or timed_out), since MySQL then falls back to the address.
     - Louhi: A and AAAA with a deadline now (its CLI's HTTPS client); later, SRV and A against
       servers named per call (one VPC's), TTL honoured, waiting on the maintainer's design.
     - Macha: nothing; its libcurl does its own lookups.
     - Mimir: nothing direct; the connector (MIGRATE's target, a name a client typed) and the
       node's seeds and advertised address on the client, the lookup inside the connect's one
       timeout, A and AAAA tried as the connect tries addresses, and a name that does not resolve
       ending the connect at once with its reason.
     - AthenaSIP: moves its own resolver onto Dannan's once it covers NAPTR, so NAPTR joins the
       types (order, preference, flags, service, regexp, replacement), with SRV's priority, weight,
       port and target; empty told apart from failed; negative answers cached (RFC 2308); RFC
       5452's checks (a fresh port per query, a reply only from the server asked with the id
       sent). It keeps a query-by-name-and-type call, so its RFC 3263 locator moves over unchanged.
       Its resolver (athenasip/src/dns/: each server in turn twice round as resolv.conf(5) does,
       TCP on truncation, the wire format with a fuzz target) is read for what it does, not copied.
     - Lugh: nothing direct once Dannan's HTTPS client takes its issuer lookups (A and AAAA, each
       address tried in turn within the caller's deadline). For outbound posts to an app's host
       (back-channel logout), a filter that sees the addresses after resolution and before the
       dial, so loopback, link-local and private ranges the operator has not allowed are refused
       (a request to the inside is refused, never made). Negative answers cached briefly (the
       SOA minimum, capped).
   - Slow peers (8): Lir, Dagda's joiner survey.
3. **The stack's Boost** (the maintainer's yes, 2026-11-24, on AthenaSIP's proposal; settled
   with Dagda): the stack moves past Debian 13's 1.83 to **1.92.0**, current and stable (released
   August 2026), pinned exactly. Dannan builds and packages it, as the base: Boost's headers only
   (Asio, Beast, MQTT5, and Boost.JSON's source, which libdannan compiles once), as a Debian
   package of the stack's own, installed beside Debian's libboost-dev under a prefix of its own
   with Boost's CMake config, in the tomcully.com apt repository; built from the release tarball
   checked against Boost's published SHA-256. libdannan-dev then depends on it rather than
   libboost-dev, DANNAN_BOOST_MINIMUM becomes 1.92.0, and CI and the Pis install it. Boost types
   are in Dannan's ABI, so it rides a minor: **0.14.0**, with atomic commit, so the stack repins
   once. Dagda follows on that Dannan (about a day and a gate), then AthenaSIP drops its vendored
   MQTT5. The package is built and staged before 0.14.0's work begins, so each project can try it.
4. **0.14.0, target mid-December:** the Boost move above, and atomic commit across replica sets (26): Kvasir step 3 and
   every multi-key write in Mimir. Its intents live in register values, so it needs 2. Its design
   (Mimir's proposal with Kvasir's corrections) goes to both as soon as the register layer's shape
   is fixed, and its code starts the day 2 is on develop, before 0.13.0 is tagged (the maintainer,
   2026-11-24); earlier than the plan's "after 3".
5. **Then:** selective channel interest (31: Lir), and the
   stack plan's third minor (3, 4, 5, 22). After those, the earlier list, unless a waiting
   project says one blocks it: DTLS and UDP for `dannan::listen`; the peer-witnessed high-water
   mark (it also covers the hybrid clock's restore without custody); loader promotion at the peer
   (its design to Macha first); and Kvasir's sync-free current read (needs its simulator run of
   crashes between the read's promise and a lower-ballot accept).

The split of the plan's second minor into 0.12.0 and 0.13.0 is the maintainer's ruling
(2026-11-23, through the coordinating session): sooner pins for the most-waited items, at one more
repin for each consumer.

**Documentation: a gate and a backfill** (the maintainer's ruling, 2026-11-23, through the
coordinating session). From 0.12.0 no release is tagged without documentation for everything it
adds or changes; in parallel the existing public API is backfilled, most used first. 0.12.0 may
slip about a day for it. The form: Doxygen comments on every public declaration (what it does,
parameters, return, errors and Outcome cases, threading, ownership and lifetime, deadlines),
rendered by a `docs` target (Doxygen a build tool only, never a library dependency), built in
hosted CI with warnings as errors so an undocumented public symbol fails it; Markdown concept
guides in docs/ with the why as well as the what; real programs under examples/, built in CI
and run as tests, quoted by the guides. No identifying data in any example. The order:
1. The machinery: docs/Doxyfile.in, `-DDANNAN_BUILD_DOCS=ON` and a `docs` target, `tools/ci.sh
   docs` and its CI job, examples/ with `-DDANNAN_BUILD_EXAMPLES=ON`, each example run by a test.
   The Doxygen input is the list of headers held to the gate, growing with the backfill, so the
   gate is whole for what it covers from the first day. Done, in 0.12.0.
2. 0.12.0's own additions, under the gate: readiness and health, RateLimiter and Backoff, the
   stored password, ProcessCommands, the node-program helper, TimeWait and TimeSource::watch,
   unix_us, Answer::hold and the per-peer share, placement's health, BufferPool's bound, and
   Delivery::ordered. Done, in 0.12.0.
3. The backfill, first part (target 2026-11-26, on track): node.h and NodeConfig, context.h
   (Outcome, Fault, Routes and Delivery are declared there), config, the node settings and
   docs/config.md, contract/completion.h, net/fanout.h, the clocks and TimeSource, BufferPool and
   the HLC are on the gate (docs/CMakeLists.txt), shipped in 0.12.0. Left: net/routes.h.
4. The concept guides, with the first part: the model (nodes and identity, membership and the
   agreed roster, founding, the clocks, the delivery kinds, settings, faults and evidence). Done,
   in 0.12.0 (docs/guide/).
5. The examples, with the guides: founding a cluster of three, routes and message types, each
   delivery kind, a hosted system on Context, settings, deadlines and Outcome, the simulated
   nodes in a test. Done, in 0.12.0 (examples/, run by dannan_tests).
6. The rest of the public API, header by header, each joining the gate's input as it is done.

**Outcomes of earlier open items:**
- LevelZero.AChunkLostOnTheWayToTheFarEndEndsItWithinTheBound (hosted ASan, 0.10.2): found and
  fixed in 0.11.0. A circuit end's close sent while its session to the relay was gone was dropped;
  it now goes by CONTROL (CHANGELOG 0.11.0, "Fixed").
- `found()`'s deadline per slot (with its node gone, a founding's outcome took one deadline per
  window of 16 slots): the maintainer ruled one deadline for the whole founding (2026-11-23). It
  rides with 0.12.0: a test that a founding whose node is gone ends by its one deadline, then the
  change.
- Still open, unseen since: LevelZero.AChunkLostWithTheSendersSessionEndsTheCircuitWithinTheBound
  (once, CI run 43900791401) and ChildProcess.ChildrenStartOnManyThreadsAtOnce's TSan hang (once,
  CI run 44116803795). Each is pinned before anything is changed; the process runner (23) replaces
  the fixture the second runs on.
- Two Dependabot pull requests (actions/checkout 4 to 7, codecov-action 5 to 7), held.

**Boost:** Dannan builds against Debian 13's Boost 1.83 (the floor, `DANNAN_BOOST_MINIMUM`), and its
packages are built with it. `dannan/json/json.h` is public and is Boost.JSON's own types: a consumer
uses the Boost headers Dannan was built with and links Boost.JSON's compiled part only from
libdannan, never its own copy (told to AthenaSIP, 2026-11-23).

**Dependencies:** read Boost's, OpenSSL's and yaml-cpp's newer releases against Dannan's code (the
maintainer, 2026-11-20); AthenaSIP's Boost 1.89 is the first case.

**Release mechanics learned 2026-11-16:**
- The `.deb`s CI builds before the tag exists come out versioned `<last>+gitN`. After pushing the tag, re-run
  `main`'s whole run (`gh run rerun <id>`; re-running a single deb job did not replace the arm64 artifact), then
  download that attempt's artifacts by id.
- A release's local gate is release, ASan and TSan on a Pi; the macOS gate only if the maintainer allows it.

**Standing, from the maintainer's rulings (2026-11-15):**
- Anything that can use agreement waits for it: no stack project builds its own consensus meanwhile.
- Lugh's TOTP, recovery codes and key rotation use agreement registers on 0.9.0 (relayed by Lugh).
- The `ack_level` names are standard: ephemeral, persisted, decided, replicated (relayed by Kvasir).
- The sim harness is built once, in Dannan (relayed by Kvasir).

**Where things are built and tested** (the maintainer, 2026-11-14: "STOP all builds on the Mac now. Use the RPi cluster only."):
- No builds or tests on the Mac without asking the maintainer first (2026-11-17, the macOS gate included). Editing, git, gh,
  reading CI logs and clang-format are fine. Before that ruling the macOS gate ran at each merge, and TSan on the
  Mac "IF AND ONLY IF bughunting" (2026-11-16).
- Every Pi boots a 4K-page 48-bit kernel, so ASan, UBSan and TSan run on all four (no clang on node-d). Node-d has
  4 GB: a sanitizer build there is -j1, after `free -m`.
- Hosted CI (`.github/workflows/ci.yml`) is the gate on Debian x86-64 and arm64 (release, clang, ASan, TSan,
  coverage, abi, deb, package), with `abi` against the minor's last tag. To run it on a feature branch, add a temporary commit putting the branch in
  `on.push.branches`, and drop it before the merge with `git rebase --onto X^ X`. Its logs come down with
  `curl` and `gh auth token` into `build/ci/` (`gh run view --log` waits for the whole run). CodeQL is a separate run
  on the same commit: don't take it for the tests.
- The Pi 5 cluster: node-a, -node-b, -node-c and -node-d, reached as `ssh root@<ip>`. Claim cores on the
  claim board first and release them when the job ends. Use nice 19 and a modest `-j`.
- live-a and live-b, which run Macha: anything on them, the cutover included, needs the maintainer's word.

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
`build/deploy.sh` and the rollback directories. The inventory lists the
rest, with how each is proven, and a port step that cannot name the proof
for a behaviour it touches does not happen.

**Dannan is written from the design, not transcribed from Macha.** Its
shape, names, namespaces, style and tests are its own from the first file.
Macha's code is read for what it does, where the inventory points, and
never copied; a Dannan test comes from the design, the laws and the
inventory row, and is written before the code. Where the design and
Macha's behaviour differ, the behaviour is the plan (the maintainer, 2026-11-05).

The wire is Dannan's own, versioned from 1 (Decisions, 2026-11-03 night):
nothing of protocol 23 or layout v18 binds it, and "No byte moves" and the
protocol-23 two-process test went with that decision. What must not move
now is Dannan's own wire across its releases: a cluster upgraded node by
node mixes versions, as the 0.5.3 and 0.9.2 window does.

## Waiting on the maintainer

Each stops something; a session that has lost its context sees them
first.

Each stops something. "Resume here" lists the current asks; these stand longer:

- **Held until he says:** anything on live-a or live-b, the cutover included; the Pi baselines and
  measurements; the real IGD run. Each push and release is his word too (`origin` is
  `git@github.com:tomdionysus/dannan.git`).
- **The AthenaSIP port (step 11): not yet** (the maintainer, 2026-11-04). The Macha move to Dagda is the one job.
- **Cloud providers and budget for the chaos cluster**, when level 0 is ready to be tried for real. Until then: the
  compose cluster only (the maintainer, 2026-11-03), and no cloud spend.
- **Under discussion with the Macha session, from the maintainer's 2026-11-05 ruling on disk limits:** Dannan's
  `sched::RetainedMemoryLedger` and Dagda's `DataResourceArbiter` and `IoReserve` look like one admission mechanism
  written three times, with different policies. It is not for now.

The first cutover's settled rulings (2026-11-05: deadlines, thread rows, DIAG lines, bounds, disk limits) and the
earlier settled questions are in `COMPLETED.md` ("Moved here from ACTIVE at the 2026-11-19 rationalisation", and
"Settled questions moved from ACTIVE, 2026-11-18").

## How to prove it

Two gates, and both are run.

**Dannan's own gate**, every commit here.
- Hosted CI on every push: Debian 13 x86-64 and arm64, with release, clang, ASan, TSan, coverage, abi, deb and
  package jobs.
- Locally, on a claimed Pi: `dannan_tests` (`-DDANNAN_BUILD_TESTING=ON`; run the binary, not `ctest`), with the
  counts recorded in `COMPLETED.md` and never lower. A test is never deleted. Time is injected and nothing sleeps.
- Sanitizers before a release, or to hunt a bug that cannot be pinned any other way: ASan with
  `ASAN_OPTIONS=detect_container_overflow=0`, TSan with
  `TSAN_OPTIONS=suppressions=$PWD/dannan/test/tsan-suppressions.txt`.
- A wire change is also run against the previous release's `dannan-node`, as `build/mixed-version.sh` does for 0.5.3
  against 0.9.2.

**The port gate** is Macha's, in `../macha` on the Pis: its suite with counts equal or above, the config golden,
the Status golden, the route list, the plugins, the install, the diff reviewed with every content change named, and
the inventory updated. The live nodes are never both down at once. Macha's runbook adds the two-version smoke test
before the first node.

For AthenaSIP (step 11, not yet): `./build-tests/athenasip_tests` with Redis and MQTT set, then
`test/e2e/cluster.sh` with the second node behind CGNAT and no public address (`docs/testing.md` there).

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
- (2026-11-02, design; the maintainer, 2026-11-23) **Dannan serves no DNS.** Manannan is a full
  cluster DNS server, its own project; no project depends on it. Dannan does get a **DNS client**,
  better than the system's lookup calls, that works with any DNS servers (the system's configured
  ones by default, or servers named) and never relies on Manannan.
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

- **No preferred node** (the maintainer, 2026-11-23, through the coordinating session). A job takes
  whichever Pi has the cores free now, never queues for one because it holds a warm build tree or
  an earlier build: rsync the tree there and build from scratch. A missing build tool, compiler
  or -dev package is installed there (one core claimed, `nice -n 19 apt-get install -y`, a board
  note naming the package and the Pi). Wait only when no machine has the cores, naming every
  machine (`claim-board wait --any`). The one exception is a job that must run on one machine (a
  service or instance that lives there, or jupiter for TSan with clang 19), said in the claim's
  job text.
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
