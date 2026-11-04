# Dannan - Completed Work

What has landed, newest first, with what shipped and the gate counts it
left green. `ACTIVE.md` is the plan and this is the record.

## Releases

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
