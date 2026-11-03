# Dannan - Completed Work

What has landed, newest first, with what shipped and the gate counts it
left green. `ACTIVE.md` is the plan and this is the record.

## Releases

None. Dannan has no code yet.

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
