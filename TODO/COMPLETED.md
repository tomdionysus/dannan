# Dannan - Completed Work

What has landed, newest first, with what shipped and the gate counts it
left green. `ACTIVE.md` is the plan and this is the record.

## Handoff, 2026-11-14: P6's remaining three, for 0.8.0

On `feature/owner-test-seam`, merged into `develop`. 0.8 is opened first (`e839322`: CMake 0.8.0, `libdannan0.8`),
since every item changes the ABI.
- **The owners' test seam.** `NodeConfig::call_fault` (from `net::ClientOptions::call_fault`) is asked about each call,
  with the peer's id when known (a call by address to a peer met before has it), the address and the type. It answers
  `net::CallFault::refuse(why)`, under which the call fails at once, or `stall()`, under which it is held unanswered.
  Neither puts a byte on the wire. A held call is idle from when it was made and is watched as a placed one is (the
  watchdog now takes an idle reading and a cancel, not a session). It ends by its no-progress watch, its deadline, a
  cancel, stop, or `Node::release_stalled_calls(peer)` ("stall released"). `Node::stalled_calls()` is the count held
  now, never cumulative (Macha counts its own where it needs one). `dannan/test/fixtures/bare_peer.h` (installed):
  `test::BarePeer`, a node with nothing hosted, with a chosen id, failure domain, name, adverts, telemetry fields and
  routes, and its own io threads. It destroys its node before stopping them, since a node's stop waits on work it
  posts there; the first CI run hung for want of that. It replaces Macha's `NodeRuntime::CallFault`, `StalledCalls`
  and `tests/stand_in_peer.hpp`.
- **The advert watch.** `Nodes::watch_advert(key, at_most_every, executor, handler)` gives `AdvertChange{node, value}`.
  First come the values of every active peer carrying the key, then each change, a peer's values at most once a
  period, coalesced to the latest and told by the next heartbeat after it (`topology_changed`, which runs every
  heartbeat). A withdrawal or a leave is never held back or coalesced away (Macha's review, `86a6fcc`). The roster
  watch already told advert changes as `changed`, unthrottled; Macha keeps its metadata generation there.
- **Non-blocking logging.** `status::AsyncSink(inner, capacity)`: `write()` queues and returns, and a supervised
  `log-writer` thread writes the queue through in order. A record past the bound is dropped and counted (`dropped()`),
  and the count is written through as a WARN line, scope `log`. `flush()` waits for what was queued. A record keeps
  the time `write()` took it, through the new `LogSink::write_at` (default: `write`), which `ConsoleSink` and
  `JsonSink` stamp with (Macha's review, `f27c746`). The writer is supervised because
  `SupervisedThreads.EveryThreadIsStartedSupervised` demands it of every library thread.
Tests: eight new (four for the fault, the advert watch, the bare peer, two for the stage). Gate: hosted CI run
43548808618, every job green on Debian x86-64 and arm64 and macOS, ASan, UBSan and TSan, `abi` against 0.7.0 skipped
as the minor's first; 673 tests, 672 passed and the one long-standing skip. Never built on the Mac (every Pi was
claimed; hosted CI was the gate). Not yet measured on the Pis: the stage's effect on io under a slow journal
(`2026-11-13-stack-needs.md` P6.7 asked for that measurement first).

## Handoff, 2026-11-14: 0.7.0

The first batch of the stack's needs (`2026-11-13-stack-needs.md`):
- the deferred reply (P8), accepted by Dagda's bench: replicas group 511 to 512 calls per sync, against 2;
- P2, the partition reading, reviewed by Lir and Lugh; Tom kept joins as built and the limit is documented;
- Macha's small fixes, reviewed by Macha; two were withdrawn;
- the cancel-before-placement fix and the timing-before-answer fix.
Tom, 2026-11-14, directly: tag 0.7.0 with what's done; the rest of the batch is 0.8.0. Built and tested on hosted CI
only (Tom stopped Mac builds the same day, and every Pi was claimed): every job green on each branch and on develop.

## Handoff, 2026-11-13: 0.6.0

The ABI promise (Tom, 2026-11-13): the SONAME and package are `libdannan.so.0.6` and
`libdannan0.6`, the CMake package `SameMinorVersion`, `dh_makeshlibs -V`, and
`tools/ci.sh abi` in CI on both architectures. The step was proven in a trixie
container three ways: the tree against 0.5.6 passes, a field added to a public struct
fails, and an added function passes. Also the `ChildProcess` reaper Lugh asked for: its
two tests fail without it (the orphan outlived its 12 s) and pass with it, and with
a third (eight children started on threads at once) pass on macOS plain, under ASan
and under TSan. Macha's review then found 0.5.4 had changed `libdannan.so.0.5`'s ABI
(the step with `DANNAN_ABI_BASE=0.5.3`: five functions removed and one changed; 0.5.5
matches), so at Tom's word the release is 0.6.0, under a SONAME nothing used before.

## Handoff, 2026-11-13: 0.5.6

Debian packages for the apt repository at tomcully.com: `libdannan0.5.6`, `libdannan-dev` and `dannan-tools`, built,
tested and linted by CI's `deb` job on x86-64 and arm64. The SONAME and the CMake package compatibility are the exact
version. How they are built and released: `docs/packaging.md`. No library change from 0.5.5.

## Handoff, 2026-11-13: 0.5.5

The Macha session's review of 0.5.4, each change approved by it: `Client::dialled` reads stop and takes the slot in one
section; the simultaneous open retries close at first, widening to a 5 ms mean, and a refused end listens on its punch
port until the open ends, taking only the far end and what its backlog holds. Three tests from the review; one fails on
Linux without the fix. Details in `ACTIVE.md`.

## Handoff, 2026-11-13: 0.5.4

Hosted CI (GitHub Actions on Debian 13 x86-64 and arm64 and on macOS, CodeQL, Dependabot, Codecov) and what its first runs
found: g++ -Werror had never built (fixed, renames only, one scoped pragma); `Client::stop()` missed a call on a session the
peer retired (fixed, with a test that failed before); the simultaneous open could never meet on GitHub's x86-64 loopback
(random retry); five timing faults in tests (journal steppers, lost-chunk direction, gossip read). Also `Telemetry::set_bytes`
with an empty value removes the field (found by the Macha session in the port). Details and commits in `ACTIVE.md`.

## Handoff, 2026-11-10: 0.5.3

Found by Dagda on inverbeg: the installed package's config asked for Boost 1.87 while the build accepts 1.83, so no consumer
could `find_package(dannan)` on Debian. One number now feeds both, and `test/linux/run.sh package` installs the library and
builds and runs a consumer of it (failed before, passes after). CMake only; the library's code is 0.5.2's. The macOS builds now
reach MacPorts' OpenSSL 3 past Homebrew's openssl@4 (`ACTIVE.md`).

## Handoff, 2026-11-09: 0.5.2

From the arm64 run at 0.5.1: a client stop could be held up to a call's deadline by a session retired as it stopped (fixed,
with an operation guard so a throw cannot hold stop); a test race stepping time before a write's progress was recorded; a case that
cannot run as root. The 0.5.1 macOS plain line corrected (a stale binary). Each commit reviewed by the Macha session. Gates in the
report, arm64 included.

## Handoff, 2026-11-08: 0.5.1

The relay's chunks numbered, credited and pipelined, every wait for a missing chunk bounded, the exact count only after an
orderly drain (wire v1 section 6); a session's handler run from a copy; accept faults contained; the node's loops woken under
their wait's mutex (a forced refresh had waited a heartbeat). Each commit reviewed by the Macha session. Gates in the report.

## Handoff, 2026-11-06: toward Yes

Relay flow control; the journal in Macha's words with a case per line; the inventory's partial rows; Macha's health
probe behaviour; injected time in deadlines and timers; the supervisor's sink use-after-free. Gates: macOS 622, ASan 622, TSan 622
in 3 of 4 full runs (one unexplained handshake timeout, `LevelZero.ANodeIsObservedFromAFixedLocalPort`); Debian plain, ASan,
TSan 624 each at that point; later gates and the two open items are in the report and under 0.5.0. Compose relay 5 of 5. Report: `2026-11-06-dannan-capabilities.md`.

## Releases

- **0.7.0** (2026-11-14, `main`, tag `0.7.0`, at Tom's word given directly: "Tag 0.7.0 with what's done"): see the handoff
  above. Gates: hosted CI on develop at the release's code, every job; the counts in the merge runs (Debian arm64 664,
  macOS 663 at P2).

- **0.6.0** (2026-11-13, `main`, tag `0.6.0`, at Tom's word given in advance: once hosted CI was green on every job
  and Macha and Lugh had reviewed it): see the handoff above. Gates: see the ACTIVE note.

- **0.5.6** (2026-11-13, `main`, tag `0.5.6`, pushed at Tom's word): see the handoff above. Gates, hosted CI on `develop`
  at the release's code: 646 passed, 1 skipped as root, on Debian x86-64 and arm64 under every step and in both `deb` jobs,
  lintian clean on both; macOS green; the package consumer on both architectures; CodeQL clean.

- **0.5.5** (2026-11-13, `main`, tag `0.5.5`, pushed at Tom's word): see the handoff above. Gates, hosted CI on `develop` at
  the release's code: see the ACTIVE note's counts (646 passed, 1 skipped as root, on Debian x86-64 and arm64 under every
  step; 645 on macOS, BBR skipped); the package consumer on both architectures; CodeQL clean.

- **0.5.4** (2026-11-13, `main`, tag `0.5.4`, pushed at Tom's word): see the handoff above. Gates, all hosted CI on `develop`
  at the release's code: `dannan_tests` 643 passed, 1 skipped (as root) on Debian 13 x86-64 g++ release, clang release, ASan
  and UBSan, TSan and coverage, and on arm64 release and ASan; 642 passed, 1 skipped on macOS (BBR, not offered there); the package consumer on both
  architectures; header and format check; CodeQL clean. The port moves to this tag when the Macha session bumps it.

- **0.5.3** (2026-11-10, `main`, tag `0.5.3`, pushed at Tom's word): see the handoff above. The package test passes on Debian
  (Boost 1.83); `dannan_tests` 639 on macOS plain. The library's code is unchanged from 0.5.2, whose gates stand. The port freezes on
  this tag.

- **0.5.2** (2026-11-09, `main`, tag `0.5.2`, pushed at Tom's word): see the handoff above. Gate `dannan_tests` 639 on macOS
  plain, ASan and TSan; 640 on Debian plain, ASan and TSan and on arm64 plain and ASan (one case skipped as root); compose 3 of 3;
  no sanitizer report. The port freezes on this tag.

- **0.5.1** (2026-11-08, `main`, tag `0.5.1`, pushed at Tom's word): see the handoff above and
  `2026-11-06-dannan-capabilities.md`. Gate `dannan_tests` 639 on macOS ASan and TSan (the macOS plain 639 was a stale binary;
  see 0.5.2); 641 on Debian TSan, and on Debian plain and ASan one commit earlier; no sanitizer report. The final TSan runs began at a host load under 10. One item open: the
  macOS TSan handshake timeout.

- **0.5.0** (2026-11-06, `main`, tag `0.5.0`, pushed at Tom's word): the work toward Yes; see the handoff above and
  `2026-11-06-dannan-capabilities.md`. Tagged with two items open, at Tom's word: relay bulk cases under Debian TSan, and a handshake
  timeout under macOS TSan. Gate `dannan_tests` 623 on macOS, 625 on Debian plain and ASan; no TSan race reported.

- **0.4.1** (2026-11-04, `main`, tag `0.4.1`, pushed at Tom's word;
  Tom: "continue, inform other projects when done"): `status::Readiness::declare`, a
  hosted system's planes declared before the node starts. A patch so
  `find_package(dannan 0.4 CONFIG)` takes it unchanged. Gate
  `dannan_tests` 423; pre-release ASan/UBSan and TSan 423 each, no report.
- **0.4.0** (2026-11-04, `main`, tag `0.4.0`, pushed; Tom: "commit,
  merge, cut a tag, push"): `dannan::config`, the settings reader every
  node binary shares (yaml-cpp, a new dependency, Tom's call), with
  `dannan-node` on it; percent-encoding (`dannan::encoding`);
  `identity::x25519_public`; `contract::Principal ==`;
  `Nodes::certificate(node)` and `Identity::ca_certificate()`.
  `find_package(dannan 0.4 CONFIG)`. Gate `dannan_tests` 422; both compose
  beds pass. Pre-release ASan/UBSan and TSan: 422 each, no report.
- **0.3.0** (2026-11-04, `main`, tag `0.3.0`, Tom: "commit, tag, merge"):
  identity (step 10: X.509 certificates through OpenSSL as wire version 2
  beside v1, CA-signed revocations gossiped and enforced, `dannan-ca`,
  node identity options on `dannan-node`, the scope seam), and what the
  hosted systems asked for: `contract::Unlocked`, a call cancelled at its
  deadline, `Time::source()` and `Time::executor()`, `sched::DutyShare`,
  the `membership` event count, `Nodes::absent_since`.
  `find_package(dannan 0.3 CONFIG)`. Gate `dannan_tests` 408. The first
  pre-release sanitizer run found a race in the deadline cancel (the
  cancel's own "RPC cancelled" could reach the caller before the
  deadline; `Node.ACallEndsAtItsDeadline` failed under ASan and TSan),
  fixed in `8b65dd1`; the second run clean: ASan/UBSan and TSan, 408 each,
  no report. Not pushed.
- **0.2.0** (2026-11-04, `main`, tag `0.2.0`, Tom: "Tag 0.2.0 + install"):
  wire v1 (Dannan's own, from version 1), level 0 (path classes, brokered
  punches, relays, moving up a class, the relay's DATA budget, placement
  told a peer's path class; the compose bed passes through real NATs),
  channels (notify, stream, bulk), the roster-change feed and incarnations,
  and the stack's foundations (secure memory and buffers, signing,
  Argon2id, node key custody, durable create, JSON logs, the shared
  fixtures); `find_package(dannan 0.2 CONFIG)` and `dannan::dannan`. Gate
  `dannan_tests` 378, `dagda_tests` 1. Pre-release ASan/UBSan clean;
  TSan reported no race. The first sanitizer run found a bug (a call left
  on a retired session outlived `Client::stop()`), fixed with its test
  before the tag; the second showed only a test's timing budget, widened.
  Not pushed.
- **0.1.0** (2026-11-03, `main` at `bafb91e`, tag `0.1.0`): step 1's
  vocabulary and step 2's transport on Asio, protocol 23 byte for byte
  with Macha (in process against the pinned 0.91.2 export, and two
  processes against Macha's binary). Gate `dannan_tests` 206,
  `dagda_tests` 1, `dannan_interop_tests` 8; ASan, UBSan and TSan clean.
  Not pushed.

## 2026-11-04 -- readiness planes declared by the hosted system

For Dagda (step 7a): `status::Readiness::declare(plane)`, through
`context().readiness()`, callable until the node starts
(`close_declarations()` at `Node::start`), so a hosted system declares its
own planes beside `NodeConfig::planes` and the node is still never ready
before every plane is known; after start it throws `std::logic_error`, as
`routes().add` does. `Node.AHostedSystemDeclaresItsPlanesBeforeStart`
(`7a3866c`). Gate `dannan_tests` 423. Lir builds on 0.4.0 (its settings
through `dannan::config`, its percent decoder gone); Dagda pins 0.4.0.

## 2026-11-04 -- one settings reader for every node binary; Dagda's step 6 asks

- `dannan::config` (`dannan/config/config.h`, `e375b08`), Tom's answer to
  Lir's request: Lugh's layered reader lifted into Dannan on yaml-cpp (a new
  dependency, his call). A binary declares its settings (dotted key, kind:
  value, list or toggle, flag, variable); the reader layers the defaults,
  the file (`--config`/`-c`, the binary's variable, or an installed path
  if it exists), `secrets.yaml` beside it, the environment and the flags,
  each over the one before, says where each value came from, and reports
  keys nothing reads. Typed reads refuse a bad value in the same words from
  any layer, naming it. 6 tests.
- `dannan-node` reads through it (`30ce4dc`): every flag as before, and a
  file and `DANNAN_NODE_*`. `TwoProcess.TheNodeReadsAConfigurationFile`;
  the bed's image installs yaml-cpp.
- For Dagda's step 6: `identity::x25519_public` (RFC 7748's vector,
  `fba0977`); `contract::Principal` compares with `==` (`fcd1a6f`);
  `Nodes::certificate(node)`, the certificate a peer proved in its latest
  v2 handshake with this node (none if never, by key only, or revoked), and
  `Identity::ca_certificate()` (`45477bb`). No wire change: the client's
  `established` hook now carries the whole `Established`.
- Percent-encoding (`dannan/encoding/percent.h`), Tom via Lir: "it should
  absolutely be in Dannan. everyone else should use it". Strict decoding
  (RFC 3986: %XX only, '+' literal, a malformed escape throws) and form
  decoding ('+' a space); encoding by a `PercentSet` of literal bytes
  (presets unreserved, form, printable ASCII; keep and encode adjust it),
  '%' always encoded and '+' too where a space is '+', so every encoding
  decodes back; upper-case hex out, either case in. Replaces Lir's
  `auth::percent_decode`, Lugh's `Router::percent_decode` and its coming
  encoder, and at the port Macha's five. 4 tests.

Gate `dannan_tests` 422. Both compose beds pass on this tree, the image
rebuilt with yaml-cpp (v1 562 s with the build, certificates 68 s).

## 2026-11-04 -- the compose bed over certificates alone

`ComposeBed.CertificatesAloneThroughRealNats` (`test/compose/`): the six-node
bed as a cluster of certificates alone (wire v2, `--no-cluster-key`),
`compose.certificates.yaml` laid over `compose.yaml`. The host's own
`dannan-node` (`--init-custody`, `--show-identity`) and `dannan-ca` (`init`,
`issue`) make every node's custody, the CA and the certificates first, as
an operator would; `node.sh` takes its identity into its state at first
start. Phases 1 and 2 as for v1 (one cluster, path classes, every pair's
path and echo on both lanes); then c1 is revoked (`dannan-ca revoke`),
handed to hub1 at a restart, and every other node drops c1 and keeps it out
20 s later, the other five still as before. Passed first run (152 s); the
v1 test, its setup now shared, passed again (63 s, image reused). Gate
`dannan_tests` 408 (the bed is not in it).

## 2026-11-04 -- a node's absence, kept across a restart

For Dagda (step 5, leases and takeover): `Nodes::absent_since(node)`, from
`Membership::absent_since`: for a node not heard of within dead_after, the
steady time it was last heard of; none for self, an active node, or one
not known. A restart used to reload every known node as just past
dead_after, losing how long it had been gone; it now rebuilds the steady
time from the wall time the roster already persisted (DNNMEM01 unchanged),
still never active until heard from again.
`Membership.AbsenceSurvivesARestart` fails with the old reload and passes
with the new. Gate `dannan_tests` 408.

## 2026-11-04 -- sched::DutyShare, and a membership event count

For Dagda (step 3):

- `sched::DutyShare` (`dannan/sched/duty_share.h`): a background loop's
  duty cycle by weight, from what Macha's `WeightedLoaderService` does
  (inventory: background pacing). Work-conserving while nothing contends;
  while something does, one slice then a rest of active time x
  foreground:background; weights at least one, a rest at most a day, so
  never starved; setup counted active but not charged; several workers
  charge once, at the last. Time is passed in. 7 tests.
- `status::NodeEvents::membership`: moves when a node joins or leaves the
  active roster, or its place, capabilities, path class or incarnation
  changes; never on adverts, which still move `topology`. Dagda's survey
  re-asks on it instead. `NodeEvents::dannan_kinds` (2) now bounds the
  hosted system's kinds; `NodeEvents.KindsAreBounded` counts from it.

Gate `dannan_tests` 406.

## 2026-11-04 -- a call's deadline cancels it; Time gives source and executor

For Dagda (step 3a works round both today):

- `Routes::call` whose deadline passes is now cancelled through its handle
  (`Node::place_call`, `dannan/node.cpp`): its reply slot is freed and the
  peer told, a later reply dropped. The deadline can pass before the client
  hands back the handle, so the handover and the timer meet on one strand.
  `Node.ACallFreesItsReplySlotAtItsDeadline` fails without it (the call
  stayed pending past the deadline) and passes with it (`5f14127`).
- `Time::source()` and `Time::executor()` on the Context: the node's
  injected time source and its io context's executor, so a hosted system
  takes neither beside the Context (`7c1501e`).

Gate `dannan_tests` 399.

## 2026-11-04 -- contract::Unlocked, and secure memory across fork

For Dagda (`LocalStore` drops its lock around device I/O, as Macha did):
`contract::Unlocked` in `contract/thread_safety.h` releases a held `Lock`
for its scope and retakes it, annotated so the analysis rejects touching
what the mutex guards inside (proved by a scratch compile: the one write
inside the scope is flagged, the ones around it are not). `secure_memory.h`
now says secure memory is deliberately not inherited across `fork()`: a
forked child that touches or frees a parent's `Secret` crashes. Gate
`dannan_tests` 397 (`35d14e0`, merged to `develop`).

## 2026-11-04 -- identity, slice 4: scope; step 10 done

`contract/scope.h`: `Principal` (a node, or a directory principal),
`ScopeDecision` (allowed, the generation read as evidence, why not), and
`ScopeSource` (answered from memory, never waiting), declared here as Tom
settled. `Identity::install_scope_source` (Lugh installs it) and
`Identity::verify` (every hosted system checks through it; Dagda's
write-floor replicas among them). Without a source, this node and any node
of the cluster may write, nobody else (Dagda's default). With slices 1 to
3, identity (step 10) is in code: certificates, wire v2 beside v1,
revocation held, gossiped and enforced, dannan-ca, and scope. **Gate:
`dannan_tests` 396.**

## 2026-11-04 -- identity, slice 3b: dannan-ca and the operator's flow

- `dannan-ca init|issue|revoke|show`: the CA's key in key custody under its
  directory (systemd credential or passphrase), its certificate `ca.pem`;
  init prints a new credential root to stdout only; issue prints a node
  certificate (random 63-bit serial unless given); revoke writes a signed
  revocation; show reads a certificate or a revocation file.
- `KeyCustody::create_and_open`: a new credential's root kept in memory to
  open custody at once (dannan-ca init first staged it in a file, which
  would have put it on disk in clear; caught before commit).
- `CertificateAuthority::create` over a given key.
- `dannan-node --init-custody`, `--show-identity`, `--ca`, `--certificate`,
  `--no-cluster-key`, `--revocations`, custody by passphrase variable or
  credential.
- `TwoProcess.AnOperatorCertifiesTwoNodesThatMeetOnCertificatesAlone`: the
  whole flow by the binaries: CA made, two nodes certified, meeting on
  certificates alone with echo on both lanes, a revocation written and
  shown. **Gate: `dannan_tests` 395.**

## 2026-11-04 -- identity, slice 3: the node on certificates, and revocation

- `identity::RevocationStore`: the CA's revocations only, each once,
  persisted (DNNREV01) under the state path; a file holding a forged one is
  refused rather than half read.
- `NodeConfig::ca_certificate`, `certificate`, `use_cluster_key`: a node
  proves itself by certificate (version 2), by the cluster key (version 1),
  or both while a cluster moves. At construction the certificate must name
  this node's id, be over its custody key, be the CA's and current, and not
  be revoked, or the node refuses to start saying which.
- Core type 20 `revocations` (reply 109): a node holding revocations sends
  them with every member exchange and keeps what the peer answers with, so
  one missed is learnt within a heartbeat. A new revocation closes the
  revoked node's sessions at once; its next handshake is refused.
- `Identity::certificate`, `revoke` (checked against the CA, applied, passed
  to every active peer), `revocations`; the Context's `identity()` has a
  mutable form.
- **Gate: `dannan_tests` 394**; the certificate node cases 10 times.

## 2026-11-04 -- identity, slice 2: wire version 2

`net/channel.*`: `Credentials` (a cluster key, a certificate credential, or
both; a key converts to one) replace the transport's key; a hello carries a
proof per version it can prove, each naming its cluster (1: the cluster
key's HMAC; 2: the node certificate and a signature by its key), and the
server picks the highest version both speak that the hello proves for its
own cluster. A certificate is checked both ways inside the handshake (this
CA, the receiver's clock, the node the hello or ack names, not revoked) and
kept as `Established::peer_certificate`; v2 session keys are salted with the
cluster id. The cluster id left the hello's body, so 0.2.0 nodes do not meet
later ones (nothing is deployed). `max_wire_version` is 2. The version 2
hello joins the fuzz targets. **Gate: `dannan_tests` 391.**

## 2026-11-04 -- process facts and timed spans, for Dagda

Asked by Dagda (step 1a moved Macha's sealing onto Dannan and found what it
lacked): `status/process.*` with `resident_bytes` and
`physical_memory_bytes` (moved out of telemetry's private code) and
`set_thread_name` (cut to the platform's limit); supervised threads name
themselves; `status::ObservedDuration`, a span timed into a histogram of
microseconds on the injected clock. **Gate: `dannan_tests` 388.**

## 2026-11-04 -- identity, slice 1: certificates and revocations

`identity/certificate.*` (`TODO/2026-11-04-identity.md`, sections 1 and 3):
`Certificate` (X.509 through OpenSSL: DER and PEM, the SubjectPublicKeyInfo,
the node id from a subjectAltName URI `dannan:node:<hex>`, serial,
validity, CA or not, subject, fingerprint); `CertificateAuthority` (create:
EC P-256, self-signed, CA:TRUE pathlen 0, keyCertSign; load from key and
certificate; issue a node certificate over a node's key, CA:FALSE,
digitalSignature, two years by default; revoke); `check_node_certificate`
(issued by this CA, within validity at the receiver's clock, a leaf, naming
a node); `cluster_id_of` (SHA-256 of the CA's key); `Revocation` signed by
the CA (serial zero covers every certificate of the node), its set encoded
as DNNREV01. `algorithm_of_public_key` beside `verify`. Certificates and
revocation sets join the fuzz targets. **Gate: `dannan_tests` 385.**

## 2026-11-04 -- ChildProcess for a command's process tests

Asked by Lugh, whose process tests move into GoogleTest: `ChildOptions`
(the whole environment replaced; bytes on standard input, written on a
thread of its own over a socket so a child that never reads cannot hold
the test or SIGPIPE it, /dev/null otherwise; standard output and error on a
pseudo-terminal), `read_all(deadline)` for raw output, `signal(number)`.
Existing callers unchanged. libutil linked on Linux for the terminal.
**Gate: `dannan_tests` 379.**

## 2026-11-04 -- the dagda/ stub removed

Dagda has its own repository and took what it needed (its session, step 0
done against 0.2.0's install). The stub goes: `dagda/` (the version stub
and its one test), its CMake target, `dannan-version`'s use of it, the
compose image's copy of it. Its removal also stops Dannan's build defining a
`dagda` target that would collide with Dagda's own beside it. The test
count loses that one test, by the decision that made Dagda its own project.
**Gate: `dannan_tests` 378.**

## 2026-11-04 -- the compose bed in GoogleTest

`test/compose/compose_test.cpp`, `dannan_compose_tests`, built with
`DANNAN_BUILD_COMPOSE_TESTS`: the bed's driver in GoogleTest (Tom: no
Python), through `test::ChildProcess` and the nodes' JSON status lines, the
same phases as before; a failed docker command's stderr is quoted in the
failure. `scenario.py` is deleted. Its first run found that 0.2.0's
Dockerfile did not copy the new `cmake/` directory, so the image could not
configure (the bed is opt-in, outside the gate; the tag's library is
unaffected); fixed. It then passed every phase in 5.5 minutes.

## 2026-11-04 -- the child process fixture

`dannan/test/fixtures/child_process.h` (`test::ChildProcess`), moved out of
the two-process test for Lir's process tests: stdout piped and read a line
at a time by deadline, stderr to a log, `wait(deadline)` giving the exit
code or 128 plus the signal, `terminate()`; SIGTERM then SIGKILL when it
goes. **Gate: `dannan_tests` 378, `dagda_tests` 1.**

## 2026-11-04 -- what the 0.2.0 sanitizer run found

The pre-release ASan/UBSan and TSan runs reported no sanitizer finding,
and one failing test each:

- **A bug** (ASan run, `Node.CancellingOutboundCallsReleasesAWaitingWorker`
  waited out its 30 s deadline): a session retired off the client's route
  table by reconciliation (crossing dials) keeps its pending calls, and
  `Client::stop()` closed only the sessions still on the table, so a call on
  a retired session outlived the cancellation. The client now remembers the
  sessions it retires until they close, and `stop()` closes them too.
  `Client.StoppingFailsACallOnARetiredSession` sets the case up exactly (the
  test node can decline to adopt inbound sessions) and was shown to fail
  without the fix.
- **A margin** (TSan run, `LevelZero.ANodeIsObservedFromAFixedLocalPort`):
  `observe_from` waited its timeout plus one second for its own timer's
  answer; under TSan the answer came later. It returned an error, never
  hung; the wait is now twice the timeout plus a second.

**Gate: `dannan_tests` 377, `dagda_tests` 1.**

## 2026-11-04 -- Tom's stack decisions; install targets; 0.2.0 prepared

- Tom settled the stack's questions (Decisions, 2026-11-04): tag 0.2.0
  with install targets; user message types from 512, registered freely;
  the scope seam Dannan's; Lugh's two domains; one process per node;
  CA-signed node revocation in step 10; authentication in Lir pluggable;
  HTTP each system's own.
- `first_user_type` is 512 (wire v1 section 4 updated).
- Durable writes make the directories they create owner-only (0700);
  existing ones are left alone (Lugh asked).
- CMake install and export: `find_package(dannan 0.2 CONFIG)`,
  `dannan::dannan`, headers in their tree under `include/dannan` (the test
  fixtures too; a consumer defines `DANNAN_TEST_TEMP_ROOT`), dependencies
  found by the config file, versions compatible within a minor. Proved by
  installing into `build/install-check` and building a consumer against it.
- The project version is 0.2.0.
- **Gate: `dannan_tests` 376, `dagda_tests` 1.**

## 2026-11-04 -- creating a file that is never replaced

`identity::durable_create_file`: durable_replace_file's guarantees, ending
in `link()`, which refuses an existing name atomically, so of two creators
exactly one wins; an existing file is refused with `filesystem_error`
(`errc::file_exists`) and left untouched, and no temporary is left behind.
Lugh asked; it also closed a race in `KeyCustody::create`, which checked
for custody and then renamed over it, so two inits at once could both
make custody. **Gate: `dannan_tests` 375, `dagda_tests` 1.**

## 2026-11-03, night -- JSON log lines

`status::JsonSink` beside `ConsoleSink`: one object per line (`time`,
`level`, `scope`, `message`), the same UTC time and filtering, the message
escaped as JSON requires; `LogFormat` and `parse_log_format("text"|"json")`,
`console_sink(format, level)`. Asked by Lugh; a stack need, since every node
is shipped alike. **Gate: `dannan_tests` 373, `dagda_tests` 1.**

## 2026-11-03, night -- level 0's last rules: placement and the relay budget

- `placement::Scheme::demote_relay_only`: bulk placement told of level 0
  keeps relay-only nodes out of the preferred owners while enough others
  can be, and spills to them first; off by default, so level 0 does not
  leak into placement unless asked.
- The relay: CONTROL circuits unconditional (the old cap on all circuits
  could refuse a member its control path); DATA circuits budgeted
  (`NodeConfig::relay_data_circuits`, default 32), refused past it and
  counted (`data_circuits_refused`), every byte counted per circuit.
- Level 0's rules from the plan are all in code now.
- **Gate: `dannan_tests` 370, `dagda_tests` 1.**

## 2026-11-03, night -- counting unlocked key memory

`identity::unlocked_secure_allocations()`: how many secure allocations
could not be locked into RAM since start, process-wide, so a node says once
that some keys may be swapped (Lugh asked; it used to warn so). **Gate:
`dannan_tests` 368, `dagda_tests` 1.**

## 2026-11-03, night -- fuzz targets

`dannan/fuzz/`: a target per parser of untrusted bytes, each with seeds
that authenticate, so a mutator reaches past the MAC: the hello, the ack,
a frame, the node record, telemetry sets, identity resets, a channel's
interest advert, and every level 0 and dial-back payload. Each may throw
only what its parser documents. `Fuzz.EveryTargetSurvivesMutatedAndRandomInputs`
runs every target over its seeds, 3000 mutations and 500 random inputs on
each gate (no finding). `DANNAN_BUILD_FUZZING` builds `dannan_fuzz_<file>`
binaries: libFuzzer with ASan and UBSan where the compiler has libFuzzer
(not Apple's clang; no MacPorts clang is installed), a replay driver
otherwise. **Gate: `dannan_tests` 367, `dagda_tests` 1.**

## 2026-11-03, night -- the node opens its custody

`NodeConfig::custody` names a key source; the node opens its custody at
construction (refusing to start, in the operator's terms, if it was never
made or does not open). `Identity` gains `has_custody`, `node_public_key`,
`sign` and `derive_local(label)`; `derive_key` (cluster-wide) now returns a
`Secret`. **Gate: `dannan_tests` 366, `dagda_tests` 1.**

## 2026-11-03, night -- the roster feed and the incarnation

- The node record carries the node's incarnation (its durability epoch)
  after `seen_unix_ms` (wire v1 section 3 updated); a new one counts as a
  topology change.
- `Nodes::watch(executor, handler)` / `unwatch`: the active roster's
  changes (`RosterChange`: joined, left, changed), a new watcher first told
  of every node active now; taken on every topology change and every
  membership tick, so a node silent past dead_after is reported as left.
- `Secret` and `SecureBytes` default to empty (nothing mapped), for Lugh's
  results and structs that hold one.
- **Gate: `dannan_tests` 365, `dagda_tests` 1**; node, two-process, level 0
  and channel cases 5 times.

## 2026-11-03, night -- secrets never in ordinary memory

Asked by Lugh, needed by Dagda too: `SecureAllocator` and `SecureBuffer` (a
growable buffer on secure pages), `net::BasicWriter<Buffer>` (`Writer` is
`BasicWriter<Bytes>`; a record holding secrets is encoded into a
`SecureBuffer`), `hkdf_sha256_secret` and `aes_gcm_open_secure` (derived
keys and opened plaintext into secure memory), `Secret` from a
`SecureBuffer`. `Argon2Costs::valid()` with a ceiling (2 GiB, 64 passes,
64 lanes), checked by `argon2id` and by custody before a passphrase is
asked for, so a crafted file cannot make a node allocate a terabyte.
Custody now keeps its key-encryption key and opened body in secure memory.
**Gate: `dannan_tests` 363, `dagda_tests` 1.**

## 2026-11-03, night -- channels

`net/fanout.*` and `Context::channels()`: named channels with a delivery
contract chosen at open (the plan's "Level 1"): notify (CONTROL, at most
once, dropped under pressure), stream (CONTROL, at least once, a sequence
per origin and destination, a gap refilled from the sender's bounded replay
buffer, what it no longer holds reported as `lost`, a new incarnation
restarting the sequence), bulk (DATA, acknowledged per destination).
Subscription is node-local by topic prefix, gossiped as the advert
`dannan.ch.<channel>` (covered prefixes folded, coarsened to everything
past 1 KiB), so a publish goes only where someone listens; a destination
with no route yet is reported and a route made for the next publish.
Core types 17 to 19 and reply 108. Retained stays Dagda's. **Gate:
`dannan_tests` 358, `dagda_tests` 1**; channel cases 10 times.

## 2026-11-03, night -- Argon2id and node key custody

- `identity/argon2.*`: Argon2id (RFC 9106, its test vector pinned) for the
  stack: custody's passphrase and Lugh's password hashes.
- `identity/key_custody.*`, the early slice of step 10: the node's ES256
  key and a seal root, sealed in `<state>/identity/custody` (DNNKEY01)
  under a root never on disk in clear: a systemd credential (made at
  create, handed to the operator to seal; read from
  `$CREDENTIALS_DIRECTORY` as 32 bytes or 64 hex digits) or a passphrase
  stretched with Argon2id under the file's salt and costs. Header bound as
  associated data; a wrong root and an altered file refused alike; never
  replaced; `derive(label)` gives hosted systems node-local keys.
- **Gate: `dannan_tests` 350, `dagda_tests` 1.**

## 2026-11-03, night -- signing

`identity/signing.*`, the stack's one home for signatures: ES256 (P-256,
SHA-256, 64-byte r || s as JOSE wants) and Ed25519; `SigningKey`
(generate, from PKCS#8, to PKCS#8 in secure memory, SubjectPublicKeyInfo,
sign); `verify`, which treats anything malformed as false. **Gate:
`dannan_tests` 341, `dagda_tests` 1.**

## 2026-11-03, night -- the compose bed passes; secure memory

On `feature/wire-v1` (this commit):

- **The compose bed passes** (`python3 test/compose/scenario.py`, Docker
  29.8.2, image on Arch with clang): six nodes through real NATs. The hubs
  public; a1 and d1 (endpoint-independent NAT) punchable and punched to
  each other; b1 (port-randomising NAT) and c1 (two NATs) relay-only and
  relayed to every other NATed node; every pair's echo answered on both
  lanes. router-d stopped: d1 dropped out; started: d1 back and punched to
  a1 again. hub1 (a relay) stopped: the relayed pairs moved to hub2; hub1
  back: all six as at the start. 9 punches brokered, 2.4 MB relayed. The
  first run found the bed's LANs declared `internal`, which Docker drops
  routed traffic on; they are ordinary networks with explicit routes now.
- `identity/secure_memory.*`, the stack's one home for key material:
  `SecureBytes` (pages of its own, locked, out of core dumps and forks,
  wiped on release), `Secret` (no text conversion, no stream operator,
  constant-time equality, `random`), `harden_process` (no core file; on
  Linux, not attachable).
- `CONTRIBUTING.md` states the stack's one layout and the shared-code and
  error rules for all three projects.
- **Gate: `dannan_tests` 333, `dagda_tests` 1.**

## 2026-11-03, night -- level 0 moves up; a logger through the Context

On `feature/wire-v1` (this commit):

- When two sessions compete for one route, both ends keep the better path
  (ordinary over punched over relayed), then the lower session id;
  reconciliation of an outbound and an inbound session does the same. The
  path sweep has the lower id of a relayed pair whose ends are both
  punchable try a punch every `path_upgrade_every`; the punched session
  replaces the relayed one, which retires, and its circuit closes.
- The connectivity loop ticks at least every heartbeat, so the sweep runs
  even when classification is an hour away.
- `Context::logger(scope)`: a hosted system logs on the node's one sink
  (`NodeConfig::log`) under its own scope.
- The stack is described once, in `../dagda/TODO/2026-11-03-stack.md`.
- **Gate: `dannan_tests` 325, `dagda_tests` 1**; node, client, reverse
  dial, two-process and level 0 cases 10 times.

## 2026-11-03, night -- level 0: path classes, punch and relay

On `feature/wire-v1` (this commit). Wire v1 sections 5 and 6, as amended
in the wire document's section 6 note:

- `reach/paths.*`: the punch and relay payloads; `classify` (punchable
  when two peers saw one local port at one external port, or one peer saw
  it preserved; relay_only otherwise); `observe_from` (a probe-lane
  handshake from a fixed local port, reading the ack's observed address);
  `simultaneous_open` (from the punch port, retried for the window);
  `Paths`, which makes paths on its own workers (a punch through each
  public broker, then a circuit through each relay), answers as broker,
  relay and target, pumps each circuit's end over a socketpair, sweeps
  circuits that lost an end, and counts everything (`PathStats`, in
  `Diagnostics::paths`).
- `net::Client`: `make_path` asked when neither end can dial the other;
  `adopt` runs the handshake on a made stream with the path's purpose and
  token and installs an ordinary route; `path_of`, `send`; two incapable
  nodes keep the lower id's session like any pair.
- `dannan::Node`: the level 0 types routed to `Paths`; the connectivity
  loop classifies the node (public when inbound-capable; otherwise by
  observation) and sets its candidates; `NodeConfig::path_class` forces a
  class, `punch_window`; `Nodes::path(peer, lane)` says how a route was
  made.
- **Gate: `dannan_tests` 323, `dagda_tests` 1**; level 0 cases 20 times,
  and node, client, reverse dial, two-process and level 0 cases 10 times.

## 2026-11-03, night -- wire v1

On `feature/wire-v1` (this commit). Tom approved the draft
(`TODO/2026-11-03-dannan-wire-v1.md`, "Continue please") with its
recommendations. Dannan now speaks its own wire, sections 1 to 4:

- Handshake v1 (`net/channel.*`): a version range in the hello, the
  server picks the highest common version and refuses none; labels
  `dannan/hello/1`, `dannan/ack/1`, `dannan/session/1`; the ack carries
  the observed address (`Established::observed`); a purpose byte and a
  16-byte path token inside the hello, checked by an acceptor that
  expects a punched or relayed connection (`Expected`), refused by a
  listening server. Frame magic `DN` 0x0001. Cluster key labels
  `dannan/auth/1`, `dannan/cluster-id/1`.
- Node record v1 (`membership/node_record.*`): id, host, port, failure
  domain, seen, capabilities (inbound, relay), path class, up to 16
  candidates, up to 64 adverts. The store's figures and flags are gone:
  a hosted system advertises them (`Nodes::advertise`, `withdraw`), a
  change makes the record newer and raises a topology event on peers.
- Placement takes a `Scheme` (both salts named by the caller, capacity
  from a named advert) and `with_advert` narrows candidates;
  `Nodes::placement(key, copies, scheme, only_advertising)`.
- Core message numbers 1 to 16 and replies 100 to 107, error 127,
  including level 0's (punch and relay); user types from 1024, a number
  below refused at registration.
- Files: `DNNMEM01` (the whole record per node), `DNNTEL01` (fields
  renumbered, hosted from 1024, no store fields), `DNNRST01`,
  `DNNINB01`, `.dannan.lock`; UPnP description "Dannan".
- Retired with protocol 23 by the decision: `dannan_interop_tests` (13
  cases), `tools/macha_types.h`. `dannan-node` is rebuilt on
  `dannan::Node` with an echo on each lane and JSON status lines, for the
  compose bed; `TwoProcess.ANodeProcessAndANodeHereFormOneCluster` runs
  it as a child and proves membership, telemetry and calls both ways on
  both lanes.
- **Gate: `dannan_tests` 315, `dagda_tests` 1**; node, client, reverse
  dial and two-process cases 10 times over.

## 2026-11-03, night -- step 5, part 2: the Context closed against Macha's use

On `feature/wire-v1` (this commit). Tom: Macha is the first customer; the
design is not complete until Macha can be refactored to use it.
`TODO/2026-11-03-macha-on-context-audit.md` lists every call Macha makes
into its network layer and what it maps to; this closes every gap that
does not depend on the v1 wire:

- `Routes`: types' wire rules registered before start (`add`), handlers
  bound and unbound at any time (`bind`, `unbind`; unbind waits for calls
  in flight, the handler lock held shared across each call); `call`
  returns a `net::CallHandle` (cancel, abort, promote, idle_for, before or
  after placement); `call_and_wait` for a worker thread, bounded by its
  deadline; `CallOutcome::peer` says who answered.
- `Channels`: all three broadcasts.
- `Nodes`: `snapshot`, `all_known_reachable`, `latencies`, identity
  resets (held, applied, propagated with a deadline).
- A `Telemetry` noun (local, all, views, persisted, persist, refresh);
  `readiness()`, `activity()` (the node notes each request's class),
  `observations()`, `threads()` and `diagnostics()` on the Context.
- `Node::cancel_outbound_calls`, so a worker blocked in a call is released
  before it is joined; `ClientOptions::dial_hook`, a test's seam to refuse
  a dial.
- Left for the v1 wire: the record's advert map (`advertise`), and so the
  store's figures and generation announcements.
- **Gate: `dannan_tests` 305, `dagda_tests` 1, `dannan_interop_tests` 13**;
  node and client cases 20 times over.

## 2026-11-03, night -- step 5: the node and dannan::Context

On `feature/node` (three commits):

- `dannan::Node`: the control plane assembled, as seam 2 lists it for
  Dannan's half of `NodeRuntime`. The state lock, the identity, the
  inbound resolver, the roster, telemetry, the transport's client and
  server, the control routes (ping, members, telemetry, identity_resets,
  dial_request, dial_back_probe), and three supervised loops: membership
  (seeds and known peers every heartbeat, forgetting at the horizon, a
  peer that takes no inbound connection exchanged only over its own
  session), telemetry (refreshed on demand, gossiped best effort at most
  once a second) and connectivity (the advertised endpoint, and the
  `automatic` answer by dial-back). `refuse_impossible_cluster` before
  listening; readiness by named planes (`status::Readiness`), the control
  plane first, so a node is reachable before it is ready; identity resets
  applied to roster, telemetry and client, and propagated; stop prompt,
  the transport first.
- `dannan::Context` (`dannan/context.h`), version 1, written as a spec:
  Nodes (roster, liveness, latency, placement), Routes (a hosted system's
  types before start, core types refused; calls with a deadline that
  answers first), Channels v1 (notify on CONTROL), Identity (id, epoch,
  derived keys), Budget (declare refuses a unit the budget cannot hold;
  reserve), Faults (raised, counted, handed to the sink), Time, Status
  (the surface), and events.
- Client: a call refused because its session retired or closed under it
  is routed again (a simultaneous dial's loser), its payload untouched
  since `Session::call` now moves from it only once admitted; the client
  tells the node of each session it dials; an identity reset drops the
  association and closes its routes. Found by the node's cases (a call at
  start failed with "peer channel is retiring"), committed on its own.
- Gate cases: `Node.*` (two nodes forming one cluster, a counter hosted
  system through the Context, a deadline, readiness and the phase, reset
  propagation, the dial-back answer, the budget, stopping) and
  `TwoProcess.ADannanNodeJoinsAMachaClusterAsABareMember` (Dannan's whole
  node joins a running Macha cluster through a seed: each lists the other,
  Macha's telemetry arrives, Macha's Status lists it). Macha's protocol 23
  types moved to `tools/macha_types.h`, shared by `dannan-node` and the
  interop tests.
- Also: clangd's index, committed with steps 1 and 2 before it was
  ignored, is no longer tracked.
- **Gate: `dannan_tests` 298, `dagda_tests` 1, `dannan_interop_tests` 13**;
  the node and client cases 25 times over, the two-process cases 5.

## 2026-11-03, night -- step 4c: supervision

On `feature/control-plane` (this commit):

- `supervise::Supervisor`: plugins from a directory (a missing one loads
  none; a library without the entry symbol is not a plugin and is
  unloaded untouched), the build identity checked before any call in and
  a mismatch disabled with its reason; declining is `unavailable` and
  never retried; a throw in construction or start, or a fault reported
  later through the sink (first reason wins, none after stopping), tears
  the instance down and retries under `sched::RetryPolicy` until it is
  disabled; stop runs the subsystem's stop, which may write; builtins
  supervised alike; a blocking construction cancelled through its stop
  token. The entry symbol and build identity are the hosted system's
  options, so Macha keeps `macha_subsystem_entry` and its identity; the
  context it passes is opaque to Dannan.
- `supervise::SupervisedThreads` (service loops with backoff doubling to
  a ceiling, once, escalating; statuses by name), `supervise::Registry`
  (by name and type; a faulted instance withdraws only itself),
  `StartupProgress` and `StartupStallGate`. Macha's process globals
  (thread statuses, startup progress) are objects injected per node here.
- Seven test plugins built as modules, one behaviour each.
- **Gate: `dannan_tests` 286, `dagda_tests` 1, `dannan_interop_tests` 12**;
  the threaded supervision cases 30 times over.

## 2026-11-03, night -- step 4b, part 2: reach v0

On `feature/control-plane` (this commit):

- `reach::InboundResolver`: configured is final; `automatic` starts from
  protocol 23's `connectivity/inbound.bin` (`MACHINB1`) or as capable;
  sticky (two failed dial-backs in a row to turn incapable, one success
  back), re-probed after 1 h capable or 10 min incapable, persisted on
  every answer; a corrupt file is ignored. `refuse_impossible_cluster`
  with Macha's reasons.
- `reach::DialBackResponder` and the dial-back codec (protocol 23's 44 and
  120): a fresh connection that must authenticate as the asker, one per
  asking peer per interval (429 inside it), 400 without an address.
- `reach::PublicConnectivity`: a UPnP mapping made, verified and owned
  (removed on exit), else the external address, else the configured
  endpoint; the self-probe; every status code Macha names; the work lock
  before the status lock so Status never waits. Gateway, lookup and probe
  injected.
- Written by hand rather than taking Macha's miniupnpc and libcurl:
  `reach::UpnpGateway` (SSDP, the device description, the WAN connection
  service's SOAP actions over a small HTTP client) and
  `checkip_external_ip` (HTTPS over Asio's SSL with the certificate and
  host name verified). OpenSSL's SSL component is now linked (Asio's TLS);
  no new dependency. Tested against a fake IGD on loopback and by their
  pure parsers; no case touches a real network.
- **Gate: `dannan_tests` 274, `dagda_tests` 1, `dannan_interop_tests` 12.**

## 2026-11-03, night -- step 4b, part 1: identity v1 and status

On `feature/control-plane` (this commit):

- `identity::StateLock` (an exclusive flock on `.macha.lock`, the name
  protocol 23's so Macha and a Dannan node on one path exclude each
  other), `load_or_create_node_id` (`node-id`, owner-only, durable) and
  `NodeIdentity` (key, id, a fresh durability epoch per process). Macha's
  storage-layout marker stays Macha's (the store's).
- `status::Telemetry`: protocol 23's tagged TEL3 record as ordered fields;
  Dannan fills the core fields (identity, the record's figures, process
  figures, peers, transport counts, traffic with rates over the interval)
  and the hosted system adds its own through a callback; a layout orders
  them (`protocol23_layout`, core and hosted interleaved). Unknown fields
  are kept as they came; core widths and the phase are checked.
  `TelemetryStore`: per-node ordering (sequence within a boot, a new boot
  unless over a minute older), identity resets, `recent`, `views`, the
  persisted set (at most 1024, a corrupt file ignored). Time injected.
- `status::ActivityClocks` (presence per work class, bytes per frame,
  lock-free), `status::NodeEvents` (monotonic counts; `topology` is
  Dannan's, a hosted system names its own kinds, at most 32),
  `status::Surface` (the design's Status noun: `publish`, `counter`,
  `snapshot`), and observations (log-linear histograms, a bounded
  registry injected per node rather than a process global, one JSON line
  per window and event, a rotated log, a recorder thread).
- Interop: `Interop.TelemetryRecordsCrossBothWaysByteForByte` (a full
  Macha record decodes and re-encodes to the same bytes; one Dannan builds
  in the layout equals Macha's) and `Interop.ObservationLinesAreMachas`
  (windows, events and route labels, escapes included).
- **Gate: `dannan_tests` 256, `dagda_tests` 1, `dannan_interop_tests` 12.**

## 2026-11-03, night -- step 4a: membership and placement

On `feature/control-plane` (this commit):

- `contract::WallClock` beside the steady `TimeSource` (wall dates
  recorded and gossiped, never deadlines), with `test::SteppedWall`.
- `membership::IdentityReset` (key, endpoint and node matching, protocol
  23's `MACHIDR1` gossip codec) and `membership::Membership`: direct
  liveness counts and gossip does not; the newer record wins and a
  direct observation of an older one only refreshes liveness; forgetting
  at the horizon (never under twice `dead_after`) with old gossip unable
  to teach a node back; the roster persisted as `MACHMEM4` (1 to 3 read),
  encoded under the roster lock and written after it in sequence order;
  identity resets applied, persisted as tombstones and honoured on load;
  flags with unknown nodes capable; `all_known_reachable`. Time injected
  throughout.
- `placement`: rendezvous, capacity placement over the 2^32 shard space
  (water level, quotas, systematic sample, failure domains, weighted
  fallbacks; protocol 23's salts by default, a hosted system may pass its
  own), logical capacity, commit-replica order, the extent candidate
  filter.
- Interop: `Interop.TheRosterFileIsSharedWithMacha` (Dannan's roster
  loads in Macha's `Membership` and Macha's in Dannan's) and
  `Interop.PlacementDecidesAsMachasDoes` (400 random rosters, 20 keys
  each, 0 to 4 copies: the same owners and fallbacks in the same order,
  the same logical capacity and commit order).
- Not here, step 5's: the gossip exchange and bootstrap seeds, publishing
  the node's figures, propagating resets to peers and the client's
  invalidation of a reset association.
- **Gate: `dannan_tests` 230, `dagda_tests` 1, `dannan_interop_tests` 10.**

## 2026-11-03, night -- a handler says what it did

On `feature/user-types` (this commit), from Tom's question: a handler
returns `Handled`, one of `processed(reply)`, `failed(status, code, why)`
or `unrecognised()`. Dannan sends the reply or the refusal, answers an
unrecognised request with 501 `not_implemented` itself (a notification
gets nothing), treats a throw as a failure answered 500, and counts
failures and unrecognised messages per type in the work stats' timings.
`Client::handle_dial_request` reports through it; `dannan-node` declines
what it does not serve with `unrecognised()`. New cases:
`Scheduler.AHandlerSaysWhetherItProcessedFailedOrDidNotRecognise`,
`Scheduler.AnUnrecognisedNotificationIsCountedAndAnswersNothing`,
`Scheduler.AThrowIsCountedAsAFailure`.
**Gate: `dannan_tests` 211, `dagda_tests` 1, `dannan_interop_tests` 8**;
the scheduler cases 30 times over.

## 2026-11-03, night -- core types and user types

On `feature/user-types` (this commit), from Tom's decision (ACTIVE,
Decisions): `RouteTable::is_core`; core types cannot be re-registered and
keep their rules; an unregistered user type is allowed on either lane in
any class, and its notifications reach the handler.
`RouteTable.AnUnregisteredTypeHasTheDefaultRules` became
`RouteTable.AnUnregisteredUserTypeIsNotPoliced` (its expectation changed
with the rule, not deleted); added
`RouteTable.CoreTypesAreDannansAndCannotBeRedefined` and
`Session.AnUnregisteredUserNotificationReachesTheHandler`. The `build/`
tree was regenerated: `/usr/local/bin/cmake`, which configured it, is
gone; `/opt/local/bin/cmake` is the one now.
**Gate: `dannan_tests` 208, `dagda_tests` 1, `dannan_interop_tests` 8.**

## 2026-11-03, night -- step 2: the sanitizers before 0.1.0

On `feature/transport` (three commits), the release rule's sanitizer
runs, in `build-asan/` (ASan and UBSan, all three suites) and
`build-tsan/` (TSan, `dannan_tests` and `dagda_tests`; Macha's core is
not instrumented, so the interop suite is not run under it):

- **ASan found a real use-after-free:** `Client::stop()` posted the
  heartbeat's cancel to the health strand uncounted, so when the
  heartbeat ended its own operation first, `stop()` returned and the
  client was destroyed before the cancel ran. `Server::stop()` posted
  its acceptor close the same way. Both are now counted operations, and
  the acceptor lives on a strand so its handler and the close never run
  at once. Fixed in their own commit.
- **ASan found a test mistake:** `Codec.ATruncatedInputIsRefused` read
  through a `Reader` over a temporary. `Reader(Bytes&&)` is now deleted,
  so the mistake no longer compiles anywhere; nothing else in the tree
  made it.
- Environment, not code: ASan runs with `detect_container_overflow=0`,
  since MacPorts' gtest (libc++ 19 headers, uninstrumented) and Apple
  clang 17's instrumented code disagree on container annotations and the
  report fires inside gtest's own test registration. TSan runs with
  `dannan/test/tsan-suppressions.txt`, which suppresses only Asio's kqueue
  reactor publishing descriptor state through kevent(2), an ordering TSan
  cannot see.
- Results: ASan and UBSan, `dannan_tests` 206 three times over,
  `dagda_tests` 1, `dannan_interop_tests` 8, all clean; TSan,
  `dannan_tests` 206 three times over and `dagda_tests` 1, no reports.
- **Gate: `dannan_tests` 206, `dagda_tests` 1, `dannan_interop_tests` 8,
  all passed.**

## 2026-11-03, night -- step 2: a session's frames, byte for byte with Macha's

On `feature/transport` (this commit):

- `Interop.DannansFramesAreMachasByteForByte`: a capture of one session
  with Macha's channel as the sender. Each frame Macha writes (a reply, a
  speculative notification, a fragmented reply's two fragments, an empty
  loader reply, `session_retire`) is read raw and compared with the frame
  Dannan's `FrameSealer` writes for the same fragment under the same
  nonce: identical, header, nonce, tag and ciphertext. The nonce is the
  one random part of a frame, so the sealer takes it from a test-only
  source (`use_nonces_for_test`), over an explicit-nonce
  `aes_gcm_seal`, whose own case shows it is the random path given that
  nonce.
- With it, step 2's gate is whole: the transport table's rows as cases,
  the two-process test, and the frame capture.
- **Gate: `dannan_tests` 206, `dagda_tests` 1, `dannan_interop_tests` 8,
  all passed.**

## 2026-11-03, night -- step 2: the two-process test

On `feature/transport` (this commit):

- `dannan-node` (`tools/dannan_node.cpp`): a node with Dannan's transport
  and nothing above it. It answers `ping`, `members` (its record and the
  peers it met, protocol 23's roster form) and `telemetry` (an empty
  `MACHTEL3` set), registers Macha's message types as Macha will at step
  3, and, given `--peer`, calls through members, ping, a `dial_request`
  for the DATA lane and `get_object` over the lane the peer dials back,
  one stdout line per event.
- `TwoProcess.ADannanNodeAndAMachaNodeFormOneCluster` in
  `dannan_interop_tests` (built when `DANNAN_MACHA_BINARY` names a Macha
  binary; run against `../macha/build-macports/macha`, executed only,
  nothing in that tree written): a Macha node from a config under the
  test's directory, ready by `/api/v1/health`, and `dannan-node` against
  it. Proven: the v15 handshake both ways; Macha's roster lists the Dannan
  node; ping; Macha dials the DATA lane back on `dial_request` and answers
  `get_object` over it with its real `404 not_found`, with one dial from
  the Dannan side; Macha's telemetry arrives every second; and Macha's
  `/api/v1/status/nodes` lists the Dannan node. Ten repeats clean.
- **Finding** (ACTIVE, Waiting on Tom, item 8): without Macha's types
  registered, the Dannan node lost its session at once: Macha sends
  `get_metadata_heads` (29) as speculative on CONTROL, and an unknown type
  is control-frame only under protocol 23's rule in both
  implementations. The earlier entry's "a bare Dannan node refuses
  nothing Macha accepts" was wrong and is marked so. Two test mistakes on
  the way: the first version started the Dannan node once Macha's ports
  accepted, before its storage routes were bound (Macha answered 501),
  and passed by luck once; it now waits for health.
- **Gate: `dannan_tests` 205, `dagda_tests` 1, `dannan_interop_tests` 7,
  all passed.**

## 2026-11-03, night -- step 2: reverse dial and maintained peers

On `feature/transport` (this commit):

- A node whose record clears `inbound_capable` is never dialled. A call
  to it with no CONTROL session of its own fails at once ("accepts no
  inbound connections and is not connected") and earns the dial backoff;
  CONTROL travels both ways over the session it opened; a lane it has not
  opened is asked for with `dial_request` (a u8 lane, protocol 23's
  payload) over that session, the calls waiting for the reverse dial
  within the connect timeout ("did not arrive within N ms"), placed as
  soon as the server admits it. `handle_dial_request` answers the
  notification (400 for a lane that is not CONTROL or DATA) and dials at
  once under the backoff. An incapable node keeps both lanes dialled to
  every capable maintained peer, checked each heartbeat, so a severed lane
  comes back unasked; in reconciliation it keeps its own dials. Peer
  flags come from handshakes, calls and `note_peer`. `close_route` closes
  one peer's lane both ways.
- The inventory's transport rows now name their Dannan cases.
- **Gate: `dannan_tests` 205, `dagda_tests` 1, `dannan_interop_tests` 6,
  all passed**; the transport cases 65 times over with no failure.

## 2026-11-03, night -- step 2: health, backoff, broadcasts; two session fixes

On `feature/transport` (two commits):

- Health (`Client::start`): every heartbeat each peer with a route is
  pinged on each lane it has (a DATA lane probed, never dialled to be);
  a hung ping is abandoned after a third of `dead_after` and retried
  ("ping made no progress for N ms"); a CONTROL route with no answer for
  `dead_after` is closed ("liveness failure: ..."), exactly that route;
  an answer is proof of life however late. The round trip is smoothed
  (0.8/0.2) from CONTROL pings only, and `alive` is told of each peer
  that answers.
- Per-peer dial backoff by address and lane, 250 ms doubling to 4 s,
  cleared by a successful dial; it governs new dials only, never a route
  the peer opened. Duplicate dials of this end now keep the lower
  session id, as the peer does.
- The three broadcasts: `broadcast` (every CONTROL route, whichever end
  dialled it), `broadcast_best_effort` (skips while the route table is
  busy; proven with `hold_routes_for_test`, the seam the decisions name)
  and `broadcast_notify` (waits for the table); none dials, and the last
  two queue only where `Session::try_notify` adds no delay (64 KiB behind
  at most 1 MiB, larger behind nothing). The server's `WorkStats` now
  carry the session count (the other seam).
- **Product bugs the soak found, fixed in their own commit:** closing a
  session from another thread (the new liveness close) abandoned its
  outbound queue at once, while the writer on the strand was sealing a
  fragment of it outside the lock: a use-after-free that aborted about
  one run in fifteen. The queue is now dropped by `shut()` on the strand;
  `close()` still refuses new work and fails pending calls at once. And a
  session now sets `SO_NOSIGPIPE` itself, so a peer that goes away never
  ends the process, whatever socket it was given (the socketpair the
  regression case uses had none). `Session.ClosingFromAnotherThreadMidTransferIsSafe`
  is the regression case, 100 times over.
- **Gate: `dannan_tests` 199, `dagda_tests` 1, `dannan_interop_tests` 6,
  all passed**; the transport cases 30 times over and the client and
  server cases 80 times over with no failure.

## 2026-11-03, night -- step 2: retirement and the client's routes

On `feature/transport` (two commits):

- `Session::retire()`: protocol 23's retirement. No new call is admitted
  ("peer channel is retiring"), what is in flight is answered both ways,
  `session_retire` (12) goes once nothing of this end's is queued, and the
  session closes as "retired" once both ends have said so and it has
  drained; a peer's notice retires this end too.
- `Client` (`dannan/net/client.h`): routes per peer and lane (its own
  dials and the sessions peers opened, admitted from the server), a lane
  dialled on demand and reused, the DATA lane only for types registered
  there, concurrent cold callers sharing one dial (24 callers, one dial),
  a dial checked against the node it meant to reach, simultaneous dials
  settled on the lower id's session with the other retired (exercised in
  every round of its test: both ends dialled, each retired one), duplicate
  inbound sessions settled on the lower session id, the no-progress
  deadline per lane (the failure names the message and says "cancelled
  for retry"; zero is none), the probe dial (authenticates, closes, leaves
  no route), and `stop()` failing every pending call at once. Requests
  arriving on dialled sessions go to the server's scheduler.
- Still owed in step 2: health pings with per-peer backoff and round trip
  from pings only, the three broadcasts, reverse dial and maintained
  peers, then the two-process test against the Macha binary (which should
  also show retirement interoperating with Macha's).
- **Gate: `dannan_tests` 189, `dagda_tests` 1, `dannan_interop_tests` 6,
  all passed**; the session and client cases 100 times over with no
  failure.

## 2026-11-03, night -- step 2: the server's scheduler and the server

On `feature/transport` (`6172274`, `01168d1` and this commit):

- `RequestScheduler` (`dannan/net/scheduler.h`): `ServerLimits` are
  Dannan's; per-class workers (fast control, control, ordered one job per
  peer at a time, data with a foreground reserve), byte and job bounds per
  queue, the ordered pool's last quarter kept for control-class jobs; a
  request that cannot be queued is answered with
  `error_reply(503, "overloaded", "request queue full")` and counted;
  promote and cancel of queued jobs; `WorkStats` with timings per message
  type and per frame class. `01168d1` fixed a race in the stop test (it
  now waits for `stopping()`); `6172274` was committed while a repeated
  run showed it, a process slip now closed by checking every suite's
  return code before a commit.
- `Server` (`dannan/net/server.h`): accept on Asio, `peer_socket_options`
  (no Nagle, keepalive at 60 s then 15 s by four, BBR where the kernel has
  it) on every peer socket, the pre-auth bound (8, the excess closed at
  once), the handshake timeout (5 s) on the connection's strand, a probe
  closed once authenticated, sessions feeding the scheduler with promote
  and cancel wired through, reaping with a count, `stop()` waiting for
  every handler that touches it.
- **Gate: `dannan_tests` 176, `dagda_tests` 1, `dannan_interop_tests` 6,
  all passed**; the scheduler and server cases 100 times over with no
  failure.

## 2026-11-03, evening -- step 2: reassembly, the outbound queue, the Asio session

On `feature/transport` (`9d51b5f` and this commit):

- `MessageAssembler` (bounds in incomplete messages, their bytes and each
  message; classes rise, never fall; notifications in one frame; bytes
  charged as reassembly until the message's last copy goes), refusals as
  the APIs state them (`error_reply`, `refusal_of`, `try_later`), and the
  `OutboundQueue` (most urgent class first, equal classes by turns frame
  by frame, the limits and the small-message reserve, promotion and
  cancellation both ways including mid-transfer, notifications told
  whether they went, 0.91.3's reply rule).
- `Session` on Asio: one authenticated connection on a strand, the
  reader through the opener and the assembler, the writer one sealed
  fragment at a time; calls admitted or refused synchronously with the
  reason, completions posted to the caller's executor; inbound requests
  answered through a `Reply`; notifications; transfer controls; close
  marked at once, so nothing is admitted after it; a throwing handler
  answers 500 and the session lives (a gap the tests found: it ended the
  session); traffic by class. The async dial and accept handshakes.
- Test mistakes found and fixed on the way: a fixture whose close
  handlers outlived it, a helper whose completion reached a finished
  frame, a budget test that let the 256-message queue bind before the
  512-reply budget (the queue is the tighter limit while calls pile up
  unwritten; Macha's own note says so).
- Interop: the Asio session as acceptor answers Macha's dialler with a
  reply fragmented at a 4 KiB frame, and calls it with an even id that
  Macha answers.
- **Gate: `dannan_tests` 158, `dagda_tests` 1, `dannan_interop_tests` 6,
  all passed**; the session cases 100 times over with no failure.

## 2026-11-03, evening -- step 2 begun: the channel, proven against Macha

On `feature/transport` (two commits, `ca9ffdd` and this one):

- `dannan::identity`: SHA-256, HMAC, HKDF, AES-GCM, X25519 over OpenSSL,
  pinned to RFC 4231, 5869 and 7748 and FIPS 180 vectors; the cluster key
  derived as protocol 23 derives it (labels `macha/auth/v1`,
  `macha/cluster-id/v1`), with a hosted system's keys derived from the
  master under its own labels (Dagda's storage key is
  `macha/storage/v1`); node ids.
- `dannan::membership::NodeRecord`, protocol 23's roster record,
  grandfathered.
- `dannan::net::RouteTable`, the open route space from the first commit:
  a 16-bit type registered with lanes, frame classes and whether it is a
  notification; Dannan's numbers reserved; the retired 37, 38, 48, 124
  refused; an unregistered type has protocol 23's defaults (control lane,
  control frame), so a bare Dannan node refuses nothing Macha accepts [wrong for Macha's own types in data frames: see the two-process entry].
  Correction to the extraction document's seam 4: `session_retire` (12)
  is the transport's, so Dannan's, not Macha's.
- The channel, sans I/O (`dannan/net/channel.h`): the v15 handshake both
  ways and sealed frames, every label and byte protocol 23's. Sealer and
  opener hold the node's route table by `shared_ptr` (a reference let a
  test dangle it).
- **Interop.** `dannan_interop_tests` links Macha 0.91.2's own
  `libmacha_core`, from a pinned `git archive` of the tag built under
  `build/interop/macha-0.91.2` (Macha's repository untouched), and runs
  Dannan against Macha in one process: the key derivation and the node
  record are byte-identical; a Dannan client and a Macha server, and a
  Macha client and a Dannan server on the DATA lane, complete the
  handshake, share the session id and frame limit, and exchange frames
  both ways, each side checking them by its own rules; Macha refuses
  object traffic on a control session as Dannan's table does. Configure
  with `-DDANNAN_MACHA_INTEROP_DIR=$PWD/build/interop/macha-0.91.2`.
- **Gate: `dannan_tests` 119, `dagda_tests` 1, `dannan_interop_tests` 5,
  all passed.**

## 2026-11-03, evening -- step 1: the vocabulary

Three commits on `develop` (`7e2f414`, `aeb5113`, and this one), tests
written first from the inventory rows and the laws, then the code; each
test first failed to build for want of the code, and five of my own test
mistakes were corrected along the way (constructor argument order, three
leases dropped by temporaries in assertions, a reserve example with no
room for reassembly, and one case asserting the opposite of the
reassembly reserve's place below the waiter gate).

- `dannan::contract`: the thread-safety annotations and lock types
  (`DANNAN_*`), the injected `TimeSource`, published snapshots.
- `dannan::sched`: the five wire frame types (protocol 23's values) and
  their queue priority; four work classes, foreground in place of viewer;
  declared waits, the work context and the wait guard as an injected
  object; budgets, cursors and pages; the retry policy with time passed
  in; the retained-memory ledger (open owners: Dannan below 256, hosted
  systems above; `wake()` for cancelled waiters); `fit_to_limit`.
- `dannan::status`: log levels, the node's sink, a component's scoped
  logger; no process-wide logger.
- `dannan::net`: the wire codec, byte-compatible with Macha's.
- `dannan::identity`: durable file replacement.
- Fixtures: the stepped clock, and temporary directories created under
  the build tree (`build/test-tmp/`), never the system temp directory.
- Inventory rows covered are marked **Dannan** with their cases; owed:
  the per-account bound (step 5).
- **Gate: `dannan_tests` 78, `dagda_tests` 1, all passed**; the threaded
  ledger and gate cases 200 times over with no failure.

## 2026-11-03, evening -- step 0: the repository and its skeleton

- First commit `2a923f5` on `main` (the plan and project files);
  `develop` branched. `CLAUDE.md` is not committed: Tom's global
  gitignore excludes it.
- The skeleton: `project(dannan VERSION 0.1.0)`; shared libraries
  `dannan` and `dagda` (Dagda links Dannan, never the reverse); GoogleTest
  binaries `dannan_tests` and `dagda_tests` behind `DANNAN_BUILD_TESTING`;
  `DANNAN_WARNINGS_AS_ERRORS`; clang `-Wthread-safety`; the design's
  directory layout; `dannan-version --version` printing the repository
  version from the generated `dannan/version.h`; `LICENSE` (GPLv3) and
  `docs/principles-and-laws.md` (verbatim from Macha `a547ddf`, one
  provenance line). Fixtures: the gate and the free-port allocator.
- Every dependency from MacPorts: Boost 1.88 (`/opt/local/libexec/boost/1.88`),
  OpenSSL 3.6.5, GoogleTest 1.18.0 (Tom installed it the same evening;
  Homebrew's copy unlinked). Configure:

  ```
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TESTING=ON \
    -DDANNAN_WARNINGS_AS_ERRORS=ON -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local"
  ```
- **Gate: `dannan_tests` 7, `dagda_tests` 1, all passed**, Release,
  warnings as errors, AppleClang.

## 2026-11-03, evening -- the plan reversed: here first

- Tom: "I misplanned. It should be here first." The in-tree decision of
  2026-11-02 is superseded: Dannan and Dagda are written in this
  repository from the design, as their own libraries in their own style,
  and Macha is ported onto them piece by piece, linking a tag at each
  port step. `ACTIVE.md` is rewritten around twelve steps that alternate
  building here and porting in Macha, with the transport first (step 2)
  and Macha on it (step 3) before the rest of Dannan exists. Tom ran
  `git init` here; the first commit waits on his word.
- The morning's leftovers in `../macha` (the goldens branch
  `feature/dannan-golden-fixtures` at `6a797af`, gate 683 + 20 + 21 green;
  the unfinished two-process script; the 0.91.2 worktree and its runs)
  were deleted by the Macha session on Tom's instruction later the same
  evening. Macha's `develop` is `a547ddf` (0.91.3 `d61eb02`, 0.91.4
  `353e600`, both untagged), clean, `develop` and `main` its only
  branches. The goldens' commits stay in the reflog for about ninety days.
- Work done in Macha this morning that stands whichever way Dannan is
  built, because it is the port gate: the 21 transport proofs
  (`c71370e`, merged), the two goldens (on the branch), and the findings
  they produced (the dropped replies, fixed in 0.91.3; the example
  refused by its own loader, being fixed; the macOS flake).
- Recorded for the next session: scratch files this session wrote under
  `/private/tmp` (the harness scratchpad) are outside the filesystem
  boundary of `CLAUDE.md`; later scratch went under
  `../macha/build-macports/dannan-scratch/`. Scratch for Dannan goes
  under `build/` here from now on.

## 2026-11-03 -- the transport proofs written

- Tom: write every owed transport case before seam 0. On Macha's
  `feature/dannan-transport-proofs` (off `develop` at `6c7ec64`,
  uncommitted), `tests/test_rpc_cluster.cpp` gained 21 cases from the
  specification (docs/cluster.md, docs/configuration.md Network, the
  net.hpp contracts) and `FaultyLinks::dialled_sockets`; nothing outside
  `tests/` changed. Each passed alone and in three runs of the whole
  `rpc_cluster` group (114 of 114). Only the added lines were formatted;
  the file is not clang-format clean at the baseline (647 lines differ).
- Gate on the branch, laptop, Release, warnings as errors: **682 + 19 +
  21**, one failure, `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`,
  an existing flake (1 run in 5 on the untouched baseline source).
- Three cases failed first, all my mistakes, and were corrected:
  `write_key` writes one key per case, so "another cluster's key" needed
  its own content; a reused gate let a stuck call through; a handler
  answered DATA-lane pings with a reply that lane forbids.
- Found: **replies beyond 256 on one connection are dropped
  unanswered** (inventory, "Findings"), proven by thread dumps and counts,
  not fixed. Two behaviours need a test seam. Both are with Tom (ACTIVE,
  Waiting on Tom, 4).

## 2026-11-03 -- the transport proof column

- The inventory's "Transport and lanes" table has its proof column
  filled from a read of every candidate case body at `6c7ec64`: 7 rows
  proven, 12 partial, 1 with none, and 23 cases owed, each named in its
  row. Found on the way:
  - Read-ahead and foreground promotion (`promote_read_ahead`,
    `promote_foreground`, `RpcServer::promote_queued`) has no test; the
    inventory's citation of `test_media_playback.cpp` was wrong.
  - `invariants/test_rpc_pre_auth_admission_is_bounded` is Linux-only
    (it counts `/proc/self/task`), so the laptop gate never runs it; and
    `test_peer_sockets_use_delay_based_congestion_control` asserts
    nothing on macOS beyond `peer_socket_options(-1)` being false.
  - `TransportTraffic` is touched by no test.
  - Nothing has two peers configured with different frame limits, or a
    hello from a newer protocol.
- basemind set up in this folder (`basemind.toml`, the rules block in
  `CLAUDE.local.md`; gitignore it when this becomes a repository); Macha's
  index was current.

## 2026-11-03 -- the laptop on MacPorts, and the laptop baseline counts

- The laptop is on MacPorts for everything Macha links (Waiting on Tom,
  2, done). Installed: `boost188` 1.88.0, `libtorrent-rasterbar` 2.0.15
  (built against boost188), `ffmpeg7` 7.1.5, `yaml-cpp` 0.8.0,
  `miniupnpc` 2.3.3, `lua54` 5.4.8; `nghttp2` 1.69.0 was already there.
  Removed: Homebrew's `libtorrent-rasterbar`, `ffmpeg@7`, `miniupnpc`,
  `yaml-cpp`, `lua@5.4` (and the leaves only they used); MacPorts'
  `boost` meta-port and `boost176` (it put 1.76 in `/opt/local/include`);
  a hand-built Boost 1.91 in `/usr/local/include` and `/usr/local/lib`,
  which `macha/build` had resolved beside Homebrew's libtorrent: the
  mixed Boost behind the macOS torrent segfaults. Homebrew's `boost`
  (1.92) and `libnghttp2` are unlinked, not removed, because folly,
  watchman and Homebrew's curl use them.
- The configure line on the laptop, in a fresh `build-macports` (the old
  `build` cache names paths that are gone):

  ```
  PKG_CONFIG_PATH=/opt/local/libexec/ffmpeg7/lib/pkgconfig \
  cmake -S . -B build-macports -DCMAKE_BUILD_TYPE=Release \
    -DMACHA_WARNINGS_AS_ERRORS=ON \
    -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local/libexec/ffmpeg7;/opt/local" \
    -DPKG_CONFIG_EXECUTABLE=/opt/local/bin/pkg-config
  ```

  Every dependency resolves under `/opt/local` and every built binary
  loads only `/opt/local`, system and own libraries, except FUSE
  (`/usr/local/lib/libfuse3`, a system install, not a port). The
  "Using Homebrew libtorrent/miniupnpc" configure messages are false
  positives: `brew --prefix` exits 0 for a formula that is not installed
  (`macha/CMakeLists.txt:183-205`); the prefixes do not exist.
- **Laptop counts at `0.91.2` (`6c7ec64`), Release, warnings as errors:
  661 + 19 + 21, all passed**, the torrent suite included, so it counts.
  `run-tests.sh` is mode 100644 in Macha's tree; run it as
  `sh ./run-tests.sh build-macports`. The fi-1 counts at 0.91.2 are still
  to be recorded, a deploy-day act.
- AthenaSIP's `build` and `build-tests` caches name Homebrew's Boost 1.92
  and its removed yaml-cpp; they need a fresh configure against MacPorts
  (boost188 meets its 1.87 floor) before the next AthenaSIP build.

## 2026-11-03 -- the baseline and the decisions

- Tom landed the 0.91.1 and 0.91.2 work in Macha and tagged `0.91.2` at
  `6c7ec64` on `develop`; the tree is clean. The seams measure against it.
  Suite counts at that tag: not yet recorded.
- Tom answered the eight design questions (`ACTIVE.md`, Decisions):
  extraction proceeds on the laptop now; AthenaSIP's mechanics are
  Dannan's style, applied at the split in one commit with the rename; the
  store is Dagda; Boost.Asio from the start, as seam 0 before everything;
  head spaces carry `replicas: N` from the start; scope is looked up in
  the Lugh head with a credential fallback; TCP simultaneous open first
  and UDP later; two protocol bumps, 24 for level 0 and 25 for identity;
  the compose cluster only until level 0 is ready to try for real.
- The plan was updated to match: seam 0 added to the extraction proposal
  with its gate against the 0.91.2 binary; seam 8's replica policy; the
  bumps through the encoding policy; `<store>` replaced by Dagda.
- Three more answered later the same day: the laptop goes to MacPorts for
  everything; moved tests ride the Macha runner through the split and the
  split converts them to GoogleTest; Status is a Dannan noun, one surface
  per node. `ACTIVE.md` gained "Resume here" for the next session.
- This repository gained `CLAUDE.md`, `CONTRIBUTING.md` and
  `.clang-format` (AthenaSIP's), and Macha's `TODO/ACTIVE.md` a "Dannan
  extraction" section pointing here, above its section 1.

## 2026-11-03 -- the plan

- `TODO/` written: `ACTIVE.md` (the plan, the rules, the decisions, what
  waits on Tom), `2026-11-03-macha-extraction.md` (the seams against the
  tree at 0.91.0 plus the uncommitted 0.91.2 work, the file table, the
  seam gate, what AthenaSIP needs), `2026-11-03-macha-function-inventory.md`
  (every behaviour of the moved code with its destination and proof), and
  renderings of Tom's two design documents of 2026-11-02.
- Found on the way, and recorded as decisions in `ACTIVE.md`: the route
  table is full (128 slots, 127 in use), so the open route space must
  widen it first; two seams the designs assume but do not name (the config
  split, the `NodeRuntime` split) must precede the CMake boundary; the
  Dannan plan's "HTTP is the hosted system's" takes the extraction plan's
  HTTP seam off the critical path.
- Baseline counts not yet recorded (Next, 1): the last recorded Macha gate
  is 659 + 19 + 21 on fi-1 at 0.90.96 (2026-10-14).

## ACTIVE.md's history to 2026-11-14

Moved from `ACTIVE.md` when it was rationalised on 2026-11-14, unedited: the "Resume here" notes from 2026-11-06
to the 0.7.0 release, the first cutover's record, and the step-by-step plan ("Next") whose steps are done. The
standing sections (the one rule, decisions, principles, standing rules, waiting on Tom, parked) stay in `ACTIVE.md`.

### Resume here (2026-11-06)

**Where this stands.** Dannan's list is finished as far as I can take it without the port;
[`2026-11-06-dannan-capabilities.md`](2026-11-06-dannan-capabilities.md) is the report to the Macha session, with
its gates and what could not be shown. The answer to the goal below is still not Yes: only the port can show it.

**Tom's rulings, relayed by the Macha session, 2026-11-06.** The test: "Dannan and Dagda are generalisations of
macha's cluster and storage layer ... The fidelity point is that after that, Macha should work just as well as it ever
did." Dannan's own wire and state formats do not count against that in themselves (the Macha session's reading). The port is
built by the Macha session, once Dannan and Dagda have reported, with both frozen while it runs. A defect found in the port:
"The port will pause until the relevant agent has identified and fixed the fault, with testing." The Macha session is the
review; the review by library agents after a clear is gone. Proving it: "We will convert macnessa and inverbeg store backends
and install there to test, prove and iterate." Macha stays frozen at `9a08846` until the port starts. Torrent moves after all
of the port, so its rows stay through the first cutover. Tag 0.5.0 cut and pushed at Tom's word, with two items open. Tag 0.5.1
cut and pushed at Tom's word on 2026-11-08. Tag 0.5.2 cut and pushed at Tom's word on 2026-11-09, clean on every gate
including arm64; the port freezes on it. One item open: the macOS TSan handshake timeout (the report's Gates section).
2026-11-10: Dagda found 0.5.2's installed config asking for Boost 1.87 while the build accepts 1.83, so nothing could
`find_package(dannan)` on Debian. Fixed on `feature/package-boost-floor` (`4cc62bf`), one number for both, with
`test/linux/run.sh package`, which failed before and passes now; tagged 0.5.3 at Tom's word the same day; the port freezes on it.
2026-11-11, on `develop` for the next release (not tagged; the port stays on 0.5.3): `cba69ea`, `Telemetry::set_bytes` with
an empty value removes the field as the header says (it stored zero bytes; found by the Macha session in the port, reviewed
by it; `Telemetry.EmptyBytesRemoveTheField`, macOS plain 640), and `fba9751`, the Boost floor comment.
2026-11-12 and 13, hosted CI at Tom's word: `tools/ci.sh <step>`, the workflows, Codecov, the badges; the style guide now
asks the same of every project. g++ with `DANNAN_WARNINGS_AS_ERRORS=ON` had never built: fixed (`d7731eb`, renames only; GCC's
libstdc++ false positives ignored around `net/codec.h`'s writer alone, `58404e8`, reviewed by the Macha session). The sanitizer
steps build without -Werror (g++ warns inside libstdc++ and Asio when instrumented); macOS CI takes the runner's newest Xcode
and OpenSSL 3's headers. GitHub's runners then failed tests that pass here, each fixed at Tom's word ("No, fix them"):
- A real defect: a session the peer retired was dropped from the route tables untracked, so `Client::stop()` missed a call
  waiting on it (the Node cancel test's 30 s). Every session made a route is now held for stop (`3a530b4`,
  `Client.StoppingFailsACallOnASessionThePeerRetired`, failed before, passes after).
- The simultaneous open retried a refusal at a fixed 5 ms, so two ends out of step on loopback stayed so on GitHub's x86-64
  kernel; now a random 0.2 to 2 ms (`965f2c9`, `LevelZero.ASimultaneousOpenMeetsWhenTheEndsBeginOutOfStep`).
- Tests: the lost-chunk relay tests dropped a chunk of B's answer on a fast runner (the hook now has the direction; `5615034`);
  five journal tests stepped time before the call's write was recorded (`8f13846`, `0787a0f`); the gossip cap test read its
  record after the step (`b43cd39`).
2026-11-13, the Macha session's review of 0.5.4, tagged 0.5.5 at Tom's word ("yes, do so"; Macha moves after its 0.94.2):
`dialled` read `_stopped` and took the slot in two sections, so a stop() between could leave an unclosed route
(`57eefa8`, one section now; by construction, no seam reaches the window). The simultaneous open's retry, at 0.2 to 2 ms,
hammered a closed port (692 attempts a second; `ASimultaneousOpenToAClosedPortRetriesAtAModestRate`): now close at first,
widening to a 5 ms mean (`afc79f4`). Retry timing alone never met reliably on GitHub's x86-64 loopback, so a refused end
listens on its punch port from then until the open ends, beside each attempt, taking only the far end's punch endpoint
and what its backlog holds before each attempt (`b9f5769`, `91f959d`; `ASimultaneousOpenTakesOnlyTheFarEnd`,
`ARetryTakesTheFarEndQueuedOnItsListener`, which fails on Linux without it, through a test-only `retrying` hook). The
Macha session approved all of it. Recorded, not to fix (its reading): on giving up, a SYN completing between the last
drain and the close is reset, when both ends are at their deadline anyway. Branch CI: 646 passed on Debian x86-64 and
arm64 under every step, 645 on macOS.
2026-11-13, Debian packages, tagged 0.5.6 at Tom's word ("Yes do that now"), for the tomcully.com apt repository: `libdannan<version>` (the SONAME is the whole version, the CMake package ExactVersion), `libdannan-dev` (depends on exactly that library), `dannan-tools` (the CA, the import, the version, with manual pages); `tools/build-deb.sh`, `docs/packaging.md`, and CI's `deb` job building, testing and linting them on native x86-64 and arm64 (646 passed each, lintian clean). Built here, arm64 runs emulated on this Intel Mac, where posix_spawn reports a missing binary as started and `ChildProcess.ReadsLinesAndReportsHowItEnded` fails; that is the emulator (a C check shows it), not Dannan.
The earlier one-off `LevelZero.AChunkLostWithTheSendersSessionEndsTheCircuitWithinTheBound` (local clang, once) has not
recurred in CI; still unproven. Tagged 0.5.4 at Tom's word ("If it's ready, do a release on main") once every CI job was green.
The macOS builds here keep `-DOPENSSL_ROOT_DIR=/opt/local/libexec/openssl3` and `CMAKE_CXX_FLAGS="-I<repo>/build/ssl3-include
--system-header-prefix=openssl/"` (Homebrew's openssl@4 4.1.0 sits in `/usr/local/include`; `build/scratch/sanitizers.sh`).

**0.7.0, 2026-11-14 (Tom, directly: "Tag 0.7.0 with what's done").** It holds the deferred reply, P2, Macha's fixes,
the cancel fix and the timing fix. What remains below goes to 0.8.0: the owners' test seam, the advert-change watch,
non-blocking logging, P4, P3 and P5.

**Tom, 2026-11-13: Dannan and Dagda support Macha, Lugh and Lir as first-class citizens** ("This was the purpose of
splitting out the technologies"). The hosted systems' needs, Dannan's answers and Tom's rulings are in
[`2026-11-13-stack-needs.md`](2026-11-13-stack-needs.md). The approved work, in order, each on its own `feature/`
branch with tests first, through hosted CI, and reviewed by the session that asked for it:
1. P1, the ABI promise: `abidiff` in CI against the previous tag, the SONAME and package back to `libdannan.so.0.5`
   and `libdannan0.5`, the CMake package back to `SameMinorVersion` (released as 0.6.0, below), and `docs/packaging.md` and README to match. It
   comes first because every hosted system pins against it.
   Then the `ChildProcess` reaper (asked for by Lugh at Tom's word, 2026-11-13). A test process that crashes or is
   SIGKILLed never runs the destructor, so its children ran on for days, holding their ports. Each child gets a forked
   reaper, which runs only async-signal-safe calls and closes every descriptor but its pipe. The pipe's EOF means the
   test died: SIGTERM, then SIGKILL after 10 s. A "done" byte is written after `waitid(WNOWAIT)` and before the reap,
   so a reused pid is never signalled. It goes in 0.6.0 with the promise.
   A virtual added to a `Context` interface is an ABI change by abidiff, so P2, P6's watch and the like batch into
   the next minor (Tom, 2026-11-13, directly); changes that touch no interface go in a patch. Tom, 2026-11-13, directly:
   tag the ABI promise and the reaper once hosted CI is green on every job and Macha, and Lugh for the reaper, have
   reviewed it; then push it and stage its packages in `../tomcully-apt/incoming/dannan/<version>/`. Macha's review
   found that 0.5.4 had already changed the ABI under `libdannan.so.0.5` (against 0.5.3: five functions removed,
   `NodeConfig::relay_drop_hook` retyped), while Macha 0.94.x is live on 0.5.3. So Tom ruled, directly, that the
   promise starts at 0.6.0 (`libdannan.so.0.6`), not 0.5.7, and the interface work below is 0.7.0.
   First in 0.7.0 (Tom, 2026-11-13, directly): P8, a deferred reply. A handler may answer later from any thread, through
   the session's `Reply` (`net/session.h:69-84`), which the scheduler now hides (`net/scheduler.h:101-103`). It is
   bounded per class and by the caller's deadline, and holds no worker while it waits. Dagda's group-commit prototype:
   one call per message at 8,000/s, a replica blocking on fsync held the workers, and 309,385 of 479,993 calls were
   refused, p99 240 ms.
   Built on `feature/deferred-reply`. The pieces:
   - `net::Answer`, `Handled::deferred()`, `net::DeferringHandler`, and `Routes::bind_deferred`; `bind` keeps the
     synchronous signature, wrapped.
   - `ServerLimits::max_deferred` (4096 per work class: running plus deferred, refused at admission), and
     `Timing::deferred`.
   - An Answer dropped ungiven refuses the call with 500 "abandoned".
   - Five tests (four scheduler, one node); macOS 653 passed.
   Found on the way, not yet fixed: when a data-lane type is registered only on the answering node, a call to it
   never reaches the handler, synchronous or deferred, by endpoint or by record. After about 11 s the caller learns
   only "peer channel closed: peer closed". Registered on both nodes, it works. Proven at debug level: the caller,
   lacking the type's wire rules, sends it on the control lane. The answering node logs "message later is invalid as
   foreground on the control lane" and closes the session, failing every call on it. The check is
   `FrameOpener::header` (`net/channel.cpp:440`), which refuses the frame before its body is authenticated.
   Tom, 2026-11-14, directly: document the rule only, wire unchanged (done on `Routes::add`). The two fixes weighed:
   - At the caller: refuse a call for a type it never registered. Tried; seven node tests call types registered only
     on the answering node, on the control lane, where that works. So it would break callers that rely on it.
   - At the answering node: read and authenticate the frame, then answer it with a refusal naming the lane, keeping
     the session. This changes wire v1's treatment of a lane violation, which is Tom's to rule on.
   Meanwhile the rule stands: register every type on every node, as the hosted systems do.
   Dagda's acceptance runs (dagda-bench on the Mac, loopback, k = 2, 8,000 messages/s, 60 s), against the deferred
   reply:
   - The server side works: a replica groups 511 to 512 calls per sync, against 2 before, and holds no worker.
   - Both cases still fail. One call per message: 218,642 of 479,992 failed, p99 174 ms (not deferred: 388,496,
     333 ms). 256 logs: about 227,000 to 250,000 failed, p99 231 to 240 ms.
   - The limit is now the call path per connection, about 6,000 calls/s per peer pair. Calls are refused with "peer
     pending reply limit reached" (`max_pending_replies` = 512, `net/outbound.h:27`) and "peer outbound queue full".
   - Dagda's log needs only about 100 calls/s per replica, which already passes, so this is Dannan's stress case.
     P5 measures it before anything changes: raise the cap, make it per class, or cheapen the per-call path.
   Also found, a contract defect: `CallHandle::cancel()` says it "completes the call at once as cancelled", but before
   placement it only sets a flag, and the call completes when its dial ends (up to the connect timeout; 5 s in
   `Client.ACallIsIdleFromWhenItWasMadeNotWhenItWasPlaced`). The fix is a once-only completion shared by the handle
   and the call. Fixed on `feature/macha-fixes`: `Client::once` makes the call's completion once-only, shared by the
   call and its handle, and a cancel or abort before placement posts "RPC cancelled" through it at once. The idle test
   failed at 500 ms before the change and completes in about 60 ms after it.
   `Node.GossipWaitsOutItsCapHoweverMuchDemandThereIs` failed once on arm64 ASan in CI (run 43534279154): the gossip
   round after its one-second step never reached A. The likely cause, unproven, is that the round is
   `broadcast_best_effort`, which sends nothing while the route table is busy, yet counts as the round, and the test
   froze time after the one step. The final wait now steps time on each try, as its first wait does; the frozen
   section still shows no gossip without a step. In production that costs one interval of gossip, by design.
2. P6's seven small fixes (Macha), then the owners' test seam, the advert-change watch and non-blocking logging. Five
   fixes are on `feature/macha-fixes`:
   - `idle_for` counts from when the call was made;
   - a const `Node::inbound()`;
   - `free_port` comes from 10000 to 32767;
   - `ClusterKey::from_master` and the two labels named once;
   - `DeferringHandler` takes the request by value, so a handler moves the payload out uncopied.
   Macha withdrew two on review: #13, `connections_canonical`, which Dannan has written since 3418733 (Macha's own hook
   wrote it again), and #11, first telemetry at join, inferred and never observed. macOS: 658 passed.
3. P2, the partition reading on `Nodes`. Even is a minority; a join adds to the agreed roster and only a retire
   removes. Built on `feature/partition`:
   - `membership::AgreedRoster`, persisted (`state/membership/agreed-nodes.bin`, DNNAGR01) and seeded from the known
     roster on first use.
   - `Nodes::partition()`, `watch_partition` and `unwatch_partition`, `members()` and `retire()`; an identity reset
     retires the stale id.
   - Hosted CI green on all 15 jobs (Debian arm64 664 passed, macOS 663). Built and tested on hosted CI only: Tom
     stopped Mac builds on 2026-11-14 and every Pi was claimed.
   - Lir and Lugh approved. Both found that joins during a split can give both sides a majority. Tom, 2026-11-14,
     directly: keep it as built, and document the hazard (done in `agreed_roster.h` and on `Nodes::partition`).
     Holding joins until the roster is whole, and counting a joiner only once a majority has seen it, were weighed and
     not taken.
4. P4, the class-aware worker pool with CPU pacing, from Dagda's `store/cpu_pacing.h`. Macha's pacing cases and a
   baseline run on the nodes are its proof.
5. P3, the shared public listener below the protocols, to Lugh's and Lir's combined requirements.
6. P5, small-message figures in `dannan-bench`, here first. Two real machines: the shared Pi 5 build servers
   corvus-jupiter (10.44.1.169) and corvus-saturn (10.44.1.170), which Tom gave on 2026-11-13 for native arm64 builds
   and cluster tests. They are shared, so run at nice 19 under a directory of our own, and clean up. macnessa and
   inverbeg, which run Macha, still wait for his word.
P7, Alpine in CI, is not now (Tom). Otherwise, fix what the Macha session or the port finds. arm64 runs on inverbeg at
nice 19 with GoogleTest in `~/dannan-arm64-deps/prefix`; the tests run there as root.


**The goal (Tom, relayed by the Macha session, 2026-11-06, in his words: "development will continue
on them with macha code frozen until they can answer Yes unconditionally, the entire reason for their
existence is this.").** Does Macha move onto Dannan with no change in data, functionality, reliability
and performance? Yes, with nothing attached. A restated, bounded or tolerance answer is a No. Today's
answer is No. Dannan's reason for existing is this; Lugh, Lir and AthenaSIP do not change that. Macha
stays frozen at the live nodes' baseline, `9a08846`. What only Tom or Macha can give goes to the Macha
session as a request, once. Sent (R1 to R6): which ruled changes count as change; a port branch in a
clone at `../macha-port`; the Pi baselines; two machines for a timed run; a tag and push; the real IGD
run.

**Dannan's list toward Yes.** Done (`dannan_tests` 568 on macOS, not yet rerun on Debian or arm64): the
storage key against Macha's own output; a 65,536-node roster with every kind of reset; files restored
byte for byte; G44's cases, G45's non-std throw (custody without certificates already had
`Node.OpensItsKeyCustody`); the importer writing `dagda/extents` as `Bytes{1}`; the supervisor clearing at
stop as Macha's does; the relaxed atomics read (parity file). Left, each needing R1 to R6 or the Macha
session: Macha opening a rolled-back state; widened goldens; arm64 repeats and a clang TSan attempt; a
two-machine measurement against Macha's baselines; a timed run; every seam owned; the facade itself. Facade writes the three `dagda/*` adverts until stage one.

**Last gate before the pause:** at this commit's code, Debian 13 plain, ASan with UBSan, and TSan, 562 each; macOS ASan with UBSan and TSan, 560 each with the BBR case skipped; no sanitizer reports. Compose bed: the two original cases pass; the relay measurement does not.

**Resumed 2026-11-06.** The pause (Tom: "Pause until it lands.") ended when Macha's old-code
removal landed: 0.93.4, tag on merge `9a08846`, live on both nodes. That is the baseline now. The
diff of `808fa0f..9a08846` over the moving files was read here: nothing in the transport, membership,
telemetry or config moves. The facade gains `startup.data_storage_degraded` and `NodeEvent::storage`
on a disk coming back. The torrent is leaving Macha, so no more torrent parity than the mapping
requires. Dagda's arbiter and `IoReserve` are on `sched::ClassWaits`, on its `feature/class-waits`,
local and unmerged until a tag has it: 449 of 449 on the Mac against `415c6c4`, no case changed, not
yet run on Linux. The goldens are re-cut at 0.93.4, in
`../macha/build/macha-fixtures/goldens-0.93.4/`, compared with its `mask-list-with-cac5998.txt`.
The arm64 run, inverbeg from `415c6c4` at Tom's word, read here from its outputs: plain 562 of 562,
ASan with UBSan 562 of 562 with no reports. No TSan on arm64: g++ 14's TSan refuses this kernel's
47-bit address space at start, so that coverage is x86_64 only. Loopback, under live replication and
load about 2.5, so not a baseline: 282 to 347 MB/s at the default frame, 103 to 128 at 4 KiB, idle
ping p50 under 0.2 ms, loaded p99 6 to 9 ms, peak RSS 177 to 207 MB. Macha's own baselines wait for its
import to finish. Open: the relay defect below, the real IGD run (at Tom's word), step 11's release (at Tom's
word).

Written for a session starting from nothing. **The plan is "The first cutover"
below, and it replaces the order in "Next".** Tom, 2026-11-05, relayed by the Macha
session and confirmed in his own words ("Do what the macha server agent says"):
replan on Macha's code. The source of truth is Macha's code at the baseline above, as it is, not the 2026-11-02 design and not the older baselines. Where they differ, Macha's behaviour is the plan and Dannan is reshaped to
it. A Macha defect found on the way goes to the Macha session and is fixed in Macha
first. `../macha` is read only from here.

Dannan **0.4.1** is the last release (`main`, tag `0.4.1`, pushed at Tom's word). It
has wire v1 and v2, level 0, channels, the roster feed, the stack's foundations,
identity, `dannan::config`, percent-encoding and `Readiness::declare`. Work is on
`feature/macha-parity`, off `develop`, not pushed. Gate: `dannan_tests` 560 on macOS, 562 on Debian 13;
no sanitizer run since 0.4.1. A read-only copy of Macha is under
`build/scratch/macha-cac5998` (`git -C ../macha archive`, untracked; recreate it if
gone).

The four documents this plan rests on:

- [`2026-11-05-first-cutover.md`](2026-11-05-first-cutover.md): the file table, every
  Status key, config key, state-file byte, journal line, hosted field and message
  number, and findings F1 to F22.
- [`2026-11-05-macha-test-mapping.md`](2026-11-05-macha-test-mapping.md): 152 in-scope
  Macha cases by fate, gaps G1 to G48, seams, fixture nouns, gates by step. Proposed;
  the Macha session confirms it.
- [`2026-11-03-macha-function-inventory.md`](2026-11-03-macha-function-inventory.md) and
  [`2026-11-03-macha-on-context-audit.md`](2026-11-03-macha-on-context-audit.md):
  re-based on `00861dd`, with a note on what `cac5998` moved. Of 71 assessed rows, 44 proven, 23 partial, 1 none, 3 Macha's.
- [`2026-11-04-macha-parity.md`](2026-11-04-macha-parity.md): the nine-item list, what is
  built, what was disputed.

Dannan is one of a stack: Dagda (`../dagda`, the store), Lugh (`../lugh`,
the directory) and Lir (`../lir`, MQTT events) are hosted systems on it,
each with its own session; what each owns is
`../dagda/TODO/2026-11-03-stack.md`, and the rules for all are
`CONTRIBUTING.md`. Ogma (`../libogma`) is just a library, not part of the stack
(Tom, 2026-11-04). Lugh pins 0.4.1 (Lugh `65cf596`; it reads its settings through
`dannan::config` and decodes with `dannan::encoding`, its own copies gone); Dagda is on 0.4.1 (389 tests; it
declares its own readiness planes, every stand-in for Dannan gone,
nothing more wanted for now);
Lir builds on 0.4.0 (`lir-node` reads its settings through
`dannan::config`, and its own percent decoder is gone for
`dannan::encoding`). Tom decided (via Dagda, 2026-11-04) that
Dagda's domain keys are wrapped to an X25519 key each node derives from
custody (`derive_local("dagda/wrap/1")`), its public half advertised and
signed with `Identity::sign`, not to the certificate's P-256 key: Dannan
gives no ECDH. A peer's certificate through the Context and the X25519
public-from-private helper shipped in 0.4.0. They send what they need from Dannan as messages;
nothing shared is written twice (Decisions).

The compose bed (`-DDANNAN_BUILD_COMPOSE_TESTS=ON`, `./build/dannan_compose_tests`,
Docker running) proves level 0 through real NATs; it is run by hand.

Read in this order: this file to the end of Standing rules;
`2026-11-02-dannan-plan.md` whole; the inventory; the head of
`2026-11-03-macha-extraction.md` and its file table;
`../macha/docs/principles-and-laws.md`; then, for the step in hand, the
Macha files the inventory names for its rows.


### The first cutover

One flag day: Macha on Dannan's transport and node with Macha's own store; it moves on when its tests pass.
The Dagda disk migration is a second cutover, later. Macha's message numbers
re-register from 512 with their payloads unchanged. The port's release is whatever
Macha's `develop` is at then.

**Test for what is on this path:** does the first cutover need it? Identity's
certificates, channels, punches and relays stay as they are, tested and shipped, and
get no more work for this; Macha runs on the cluster key. The AthenaSIP port and "the
six" stay off it too.

Each step is a `feature/macha-parity` milestone, gate green, committed and merged to
`develop`. The Macha cases named are its gates: `2026-11-05-macha-test-mapping.md`,
"Gates by step", lists them all.

| Step | What | Closes | Macha gates, for example |
|---|---|---|---|
| 1. Call semantics, **done** (460) | No-progress deadline and stall notice per call, off unless asked, as Macha applies them only to synchronous `call`. `net::CallWatch` on `Routes::call` and `call_and_wait`; `NodeConfig`'s four settings now default to off, which also ends the old 30 s; the node's own exchanges get `exchange_budget`, 30 s. Ruling A keeps the required deadline, so nothing more here. | F1, F2, F16, G11-G13, G18, G19 | `test_rpc_cluster`'s no-progress and liveness cases |
| 2. Lifecycle and readiness, **done** (472) | `Readiness::hold_until_marked` and `mark_complete`; first failure kept. A job on a closed session skips its handler. A failed inbound persist no longer restarts the loop. Escalation names the thread. Every thread supervised, with a guard test. `request_stop`; the layout check before the lock. | F5, F9, F12, G16, G41-G43, G45 | `test_every_subsystem_thread_is_run_supervised`, startup cases in `test_service_root` |
| 3. Transport proofs, **done** | The end-to-end cases Dannan lacks, including the 32 MiB overtake, and forged handshakes. They found no behaviour to change except G23, now matched at Tom's word: a call on a retired dial waits for the peer's session (`place_hook` forces it in the test). `ChoosesBbrWhereOffered` runs only on Linux (step 10). | G1-G10, G14, G15, G17, G20-G23, G25-G27 | `test_rpc_cluster`'s transport cases |
| 4. Membership proofs, **done but for G33** (503) | Resets, fences, forgetting, real dial-backs, retired numbers (`Routes::retire`), one-reading snapshot. No membership behaviour changed. G33 needs an fsync hold, Linux only as in Macha, so it moves to step 10. The empty-domain fallback is the facade's (F3). | F19, F20, G24, G28-G33 | `test_absent_nodes`, `test_rpc_cluster`'s membership cases |
| 4a. The goldens, **taken** | The Macha session's config, Status, route and journal captures at `cac5998`, in `build/macha-fixtures/`, local only. Reached: at most 381 of 494 Status keys, 31 of 83 journal lines; the rest stay unproven. | the goldens the port gates on | none; the port compares against these |
| 5. The journal, **written; golden check open** (517) | The 44 missing lines: subsystems, connectivity, thread faults, inbound, endpoint, reset, forgot, exchange failures, in Macha's words and levels, reworded only where Dannan lacks the field (no `hosts_extents` in the inbound line; `UPnP removal failed` names the error, not a code). `status::ThreadCpuReporter` per pool (ruling C), reaching io threads by readings posted to the pool, so an idle one reports late. Not every line has a case; the 31 lines the golden reached are compared at the port. | F13, G44, G48 | the journal golden, step 4a |
| 6. Telemetry and Status, **Dannan's part done** (524) | The hosted-field case; the table is in the spec, adopted by Macha. Real connection counts: `ClientStats` gains created (after the identity check) and canonical; `reused` counts calls placed on a route, close to but not exactly Macha's sites. Loops renamed (ruling B); truncated records; refusal rows. Byte identity against the Status golden needs Macha's facade, so it is the port's gate, not proven here. | F14, F15, G46, G47 | the Status golden, step 4a |
| 7. The importer, **done** (532) | `dannan-import-macha`: `MACHMEM1` to `4`, `MACHINB1`, `MACHTEL3`, both lock files, `--dry-run`, safe to rerun, keeps `*.macha` for rollback; Dannan's loader checks a roster before anything changes. Ran clean on copies of macnessa's three files and a scratch node's. The hosted table and advert name are written in the tool, from the adopted table, not read from a place Macha owns. | F6, F17 | the state directory, step 4a |
| 8. Foundations proofs and the budget table, **done** | Ruling D: the table is in `2026-11-04-macha-parity.md`, every budget in Dannan's layer expressible. `sched::ClassWaits` is the shared mechanism, smaller than first proposed: waiters by class and the wait itself; rules, figures and leases stay the caller's. The ledger is on it. Dagda's rebuild is its own step, on a tag. Quota boundary, reassembly reserve, retry backstop, durable contents, every-mask tables, `Published`, Observation rows. | G34-G40 | `test_foundations`, `test_contract`, `test_observation` |
| 9. Measured gate, **tool done; no comparison yet** | `dannan-bench`: 32 MiB on DATA, CONTROL round trips under load, MB/s, RSS, over the per-call strand and timer the required deadline costs; compose-bed run. Recorded, never asserted. The laptop's numbers are not comparable to the Pis'. | parity 6 | Macha's baselines at whichever release is then deployed, when Tom says |
| 10. Linux, **done** | `test/linux`: Debian 13, g++ 14, Boost 1.83, which is now the minimum. x86_64 here, and arm64 on inverbeg without TSan (see above). It found and fixed two bugs macOS never showed: every inbound connection crashed under g++ (argument evaluation order, `net/server.cpp`), and stopping could destroy a condition variable in use (`end_operation`). It also fixed a punch failing 1 in 6 on Linux loopback, by retrying sooner after a refusal. One TSan report remains, 1 in 100 runs of level 0: Boost's `ref_count_down`, an acquire fence TSan cannot model. | parity 8, G1 | the Pis' OS and architecture; no Macha case |
| 11. Release | A tag at Tom's word, with its sanitizer runs. The port pins it. | | Macha's mapped cases green on it |

**For the hosted systems at the next tag:** `NodeConfig`'s `control_no_progress` no longer defaults to 30 s, so a call is watched only if it asks (`net::CallWatch`); `Routes::call` and `call_and_wait` gain that argument; the Context gains `wait_guard` and `refresh_public_connectivity`; `Readiness::fail` keeps the first failure; an escalated fault's text names the thread.

**The facade, not Dannan's, per the cutover spec:** the advertise-host and failure-domain
fallback (F3), building the gateway (F4), deriving Macha's events from `Nodes::watch` (F7),
keeping Macha's activity clocks (F8), `metadata_notice`'s registration (F10), `version` as
`kServerVersion` (F11), `memory_bytes`' meaning (F18), the replicas advert's reader (F21),
plugins rebuilt (F22), and the call filter that replaces send and admit seams.

**Seams:** none to build. Of the 10 cases using `FaultyLinks`, 7 test transport internals
and retire; the 3 Macha-visible ones only stall a message type leaving Macha's own node.

**Fixed 2026-11-06: relayed DATA had no flow control.** Circuits are now paced end to end by credit, carried on the DATA
lane at each hop, and handled on the session; `ComposeBed.MeasuresThroughNatsAndARelay` passes 5 of 5 (was 2 of 8). Wire v1, section 6.

**The rollback** is not "nothing on disk changed": 0.92.x refuses `DNNMEM01`, so the
runbook restores the `*.macha` files, losing only what was learned since (F6).

**Agreed with the Macha session, 2026-11-05.** The test mapping is confirmed, on
conditions: a retire takes effect only in the port commit that removes its Macha subject,
and the mapping file goes into Macha's tree with that commit; G43, G44, G45 and G48 close
before the port (G48 after ruling C); G41 covers both readiness differences. The hosted
ids (1024 to 1045), message numbers (512 plus the current number, with 549, 550, 560 and
636 unregistered), advert names and memory owners (`rpc_frame` 1, Macha's ten from 256)
are adopted as proposed. `frame_timings` keeps Macha's `read-ahead` spelling at the
cutover. The three metadata mutation types register `ordered`. Macha's 0.92.4 fixes the defects sent to it, and is the baseline. The
facade raises the remote metadata generation by compare-and-swap, as 0.92.4 does.

**Macha's code is frozen at `cac5998` until the libraries are ready (Tom, relayed by the
Macha session, 2026-11-05: "Macha has a working, tested codebase, this shouldn't be
modified until they're ready."). Macha can still produce artefacts for this plan.**

- **Step 4a: the goldens are the Macha session's to cut**, under
  `../macha/build/macha-fixtures/`, because it has the working build, the two-node
  fixture and the masking knowledge. Dannan keeps a local copy in
  `build/macha-fixtures/`, out of the repository at Tom's word (they hold macnessa's host
  paths), and checks them
  against the cutover spec's 83 lines and 494 keys. A line or key the capture does not
  reach is marked unproven, not dropped.
- **Step 7's state directory** is there too, young and small; `membership-tombstone/`
  adds a real identity-reset tombstone and an absent node. Macha writes only `MACHMEM4`; `MACHMEM1` to `3` have a reader
  (`membership.cpp:12-16`) and no encoder, so Dannan builds those samples by hand from the
  reader's layout. A copy from macnessa is better evidence and waits on Tom's word.

**Settled with the Macha session, 2026-11-05.** The baseline at `cac5998` stands: the Merkle
namespace is in it and on both nodes (tree-backed heads, SM19, re-rooted 2026-11-03), which
is what Tom means by the merge being done. The branch `feature/tree-native-namespace`
(`b080c38`) is a clean-up, unmerged, held by the freeze, and touches nothing moving here.

The macnessa copy is at `../macha/build/macha-fixtures/state-macnessa/`, three files, with
md5s, mtimes and the release in its README. It stays there, out of this repository, since it
names real hosts.

**The class admission generic** (`dannan::sched`, then Dagda's `DataResourceArbiter` and
`IoReserve` on it; agreed with the Macha session, not prototyped). It fits only if it can
express, from `cluster/data_work.hpp`:

- Three levels, asymmetric: foreground may use the whole capacity and is never refused for
  pressure; a waiting foreground closes both lower classes, a waiting loader closes only
  speculative; lower classes are bounded by the whole capacity and by capacity less reserve.
- An outside predicate for pressure: a loader is refused only when a foreground user is
  present (waiting, holding bytes or recently active), speculative by pressure alone, neither
  below `min_background` active lower leases. With no device, exactly the ungated admission.
- A count ceiling beside the byte budget.
- Waits: too large for its class fails at once; a context deadline wins; otherwise no-progress
  windows that any release re-arms, with the one `DATA credit wait abandoned` warning;
  refusals and waits counted once per waiter; cancel, expiry and stop count as cancelled.
- Control cannot enter the arbiter: it throws. A per-request `charge()` applies first.

The gate is Macha's `test_io_pressure.cpp` as moved to Dagda, with the cases the Macha session
named (`foundations`, `rpc_cluster`, `root_parts`, `torrent_disk_io`, `filesystem_fuse`).
`DataResourceStats`' fields keep their meanings: `/status/diagnostics` `data_resources` reads
them by name (`status_api.cpp:1010-1037`). Not folded in: pacing (`WeightedLoaderService`,
Dannan's `DutyShare`, where "paced, never stopped" lives), maintenance's byte credits, the
per-walk `Budget`, `DiskServiceMonitor`. If it cannot express these, it does not ship, and
that is reported rather than bent.

**From Tom, relayed by the Macha session, 2026-11-05.** The gate for the port is an independent
review by the Dannan and Dagda sessions themselves after their context is cleared (Tom: "by fresh
agents I meant the library agents, after a clear"), from the code and the nodes only, concluding no
loss of function, performance, reliability or data. Macha's order: old-code removal, then 0.92.6, then the Pi
baselines on that, then Dannan finishes on that baseline; Macha's freeze has those exceptions,
it says at each commit whether anything in Dannan's layer moves, and the goldens are re-cut
once at the end. The class admission generic is required before stage one, ledger first,
then Dagda, gated by every existing case on both sides. No soak between cutovers: a stage
moves on when its tests pass, inverbeg first. Inverbeg is inbound capable, dialled at
inverbeg.macha.network:7437.

**Pace, Tom, 2026-11-05:** run through steps 5 to 10 without asking, stopping before step 11's release. The real IGD run happens before the cutover, at his word when these steps are done. Dagda hears about the class admission generic now and rebuilds its two once it exists at step 8.

### Next

**Superseded in order by "The first cutover" (2026-11-05).** The steps below keep
their gates and their account of what was done.

In order. A build step and the port step that follows it alternate, so
the boundary is pulled on from Macha's side as early as possible. Nothing
here waits on a device, a carrier or another repository, except the items
under Waiting on Tom.

#### 0. This repository (2026-11-03)

- [x] `TODO/`: the plan, the record, the port proposal, the function
      inventory, the two design documents (the masters since 2026-11-04).
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

#### 1. The vocabulary (step 1) - done 2026-11-03

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

#### 2. The transport on Asio (step 2)

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

#### 3. Macha on Dannan's transport (port step)

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
fields of it); a deploy to the live nodes. A Macha release, numbered by the Macha session.

#### 4. The control plane (step 4, in sessions per table)

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

#### 5. The node and `dannan::Context` (step 5)

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

#### 6. Macha on Dannan's node (port step)

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
the plugin cases, the live nodes. A Macha release, numbered by the Macha session.

#### 7. Dagda (step 7, in sessions per table)

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

#### 8. Macha on Dagda (port step, the snapshot last)

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
`macha-metadata-dump` identical on a fixture state directory; the live nodes for
the period the extraction plan names. A Macha release, numbered by the Macha session. After this
the door exists, and the second hosted system comes through it and
nothing else.

#### 9. Level 0: reachability (step 9) - done, compose bed only; the chaos cluster waits on Tom

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

#### Foundations for the stack (2026-11-03 night, under "no duplicated code")

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

#### 10. Identity (step 10) - done 2026-11-04, released in 0.3.0

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

#### 11. The AthenaSIP port (step 11)

`Datastore` and `EventSystem` implemented as Dannan-backed drivers behind
the existing plugin interfaces, the SIP stack untouched; heads for its
records, notify channels for its signals, leases for trunk registration,
presence takeover and the orphan-call sweep, per-origin counters for
`counter_add`, retained status from gossip, `AddressDiscovery` retired
into level 0. Redis and Mosquitto become optional, then go. Gate:
`test/e2e/cluster.sh` with the second node behind CGNAT and no public
address. The extraction document's "What AthenaSIP needs" has the
mapping.

#### 12. The six (step 12)

Event system, KV store, git forge, compute, mail, general filesystem, in
that order. Any core change one of them needs is a step proven on Macha
and AthenaSIP first. Not planned here.

---

