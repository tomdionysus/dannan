# Dannan - Active Work

Last updated: 2026-11-20 evening, at 0.10.0's release. Dannan is written in this repository as its own library, Macha's code at `00861dd` being the source of
truth, and Macha is ported onto it (the maintainer, 2026-11-03: "here first"). `COMPLETED.md` is the record; move items across
as they land, with what shipped and the gate counts.

The design is the maintainer's, 2026-11-02, in [`2026-11-02-dannan-plan.md`](2026-11-02-dannan-plan.md) (the library; it wins
where the two differ) and [`2026-11-02-macha-core-extraction-plan.md`](2026-11-02-macha-core-extraction-plan.md)
(the seams, read as port steps). These copies are the masters (the maintainer, 2026-11-04). The Claude Docs the projects
started from are never read for direction or edited.
- The port's seams: [`2026-11-03-macha-extraction.md`](2026-11-03-macha-extraction.md), with a note at its head on
  how to read it.
- What Macha does that must survive:
  [`2026-11-03-macha-function-inventory.md`](2026-11-03-macha-function-inventory.md).
- The live cluster is live-a and live-b (Pi 5, Debian 13), on Macha 0.94.2 and Dannan 0.5.3.

## Resume here (2026-11-20 evening)

**0.10.0 is released** (COMPLETED.md). In order, next:
1. **0.10.1, a patch, ABI kept:** `agree::MemoryStore::each_held`, agreed with Mimir so it can drop
   its own store:
   `void each_held(Slot, const std::optional<Name>& after, const std::function<bool(const Name&, const AcceptorState&)>& visit) const;`
   - the walk resumes strictly past `after`, by the name's bytes, whether or not that name is
     still held;
   - the visitor returns false to stop;
   - the store's lock is held across the walk, so a visitor never calls back into the store;
   - order is stable while nothing is kept;
   - first test: a name removed between two pages is never skipped.
2. **0.10.3, a patch, ABI kept (Dagda, 2026-11-22), ahead of 0.11.0:** in-place AES-GCM, so
   Dagda serves and stores an object in one buffer (its live-a memory: glibc holding freed
   16 MiB transfer buffers; the maintainer ruled out an allocator setting).
   - `void aes_gcm_open_into(key, nonce, tag, ciphertext, std::span<uint8_t> out, associated = {})`:
     out the size of ciphertext, may be exactly it; partial overlap refused; a bad tag throws as
     aes_gcm_open does and zeroes out.
   - `struct GcmSeal { nonce; tag; }`; `GcmSeal aes_gcm_seal_into(key, plaintext, out, associated = {})`
     and the overload under a given nonce; out the size of plaintext, may be exactly it.
   - Tests first: in and out of place agree with aes_gcm_seal and aes_gcm_open byte for byte; a
     bad tag zeroes out; a size mismatch or partial overlap is refused.
   - Reads into a caller's span are already `storage::File::read_at(offset, span)`.
   - A pool of extent-sized buffers under Context::budget(): not now; only if a second project
     needs it after Dagda measures the single-buffer paths.
3. **Defect, found 2026-11-22 (level 0 hunts in hosted x86-64 ASan):** a starting node dials
   a peer's CONTROL lane twice (the test logs show two "outbound ... lane=control" per node).
   Both ends keep the same one of the pair, but while one end retires the loser the other can
   be left a moment with no route, so a call then finds "no relay" or "the relay has no session
   with the target". Find why the dials are not one (by endpoint and by id, say), prove it with
   a test, and make it one. The move-up test waits its first echo out meanwhile.
4. **0.11.0:**
   - the stream carrier (feature/stream-carrier, for Lir): its memory-pipe write test fails, and a
     run ended in a bus error;
   - item 6 (feature/clock-rules), its fixes re-gated;
   - JSON, the Boost.JSON half of item 24, moved here from the second minor (the maintainer,
     2026-11-22, in Lir's session; HTTP stays in 0.12.0). Boost.JSON becomes a Dannan
     dependency, its compiled unit carried once in Dannan. Lir builds objects of strings, int64
     and uint64 kept distinct, booleans and arrays, serialised to bytes; it parses nothing yet
     (later: HTTP publish bodies, its rules engine).
   - then the plan's second minor (2026-11-20-stack-plan.md).
   - Agreed with Dagda for its matching minor:
     - `Outcome<T>::failure(Fault, message, T evidence)` keeps the evidence beside the fault;
     - `Fault::cancelled`, added last;
     - timed_out's text: "the outcome is unknown, and a write may yet land";
     - Dagda carries AckLevel's four names, plus `durable` as its own.
   - Whether a store may offer only some levels is Kvasir's ruling (Dagda asked).
   - dannan-node follows docs/config.md:
     - its flags are made from their keys, so `--port` becomes `--listen-port`;
     - its variables take the project's prefix, so `DANNAN_NODE_...` becomes `DANNAN_...`, and
       the README's example changes with them;
     - it takes `log.level` and `log.format`, the stack's shared keys.
     The flags are part of its interface, so the change waits for the minor.
   - Ship `dannan/test/tsan-suppressions.txt` (Dagda's ask; Asio's executor count, a false
     positive every TSan build on Asio can hit). Steps:
     - libdannan-dev installs it;
     - the CMake package sets `DANNAN_TSAN_SUPPRESSIONS` to its path, installed or fetched;
     - docs/building.md tells every TSan run to pass it, so the stack keeps one list.
   - A TimeSource that tells its waiters when it is stepped, so a stepped or frozen clock arms
     no timer until it moves. Today every wait on it re-arms each `wake_within` (5 ms by
     default), and each pending call's watchdog is one such wait. With thousands of calls and a
     frozen clock, that churn starved a shared io_context under TSan (Dagda's
     PromptReplication stalls, 2026-11-20). Confirmed as starvation, not loss: a stalled run
     left going counted 1480 refusals at 150 s and 1789 at 300 s. Dagda wraps its clocks at
     250 ms meanwhile. In the next release, before that: `test::SteppedTime(poll = 5ms)` takes
     its wake_within poll as an optional constructor argument, so a fixture with many waiters
     can poll coarsely. Dagda wraps it meanwhile.
   - Deferred requests bounded in bytes. This is the likely Dannan side of Macha's live-a OOM after
     the cutover, 2026-11-21 and 2026-11-22: about 6 GB against a 768 MB budget, right after Dagda
     DATA device-pressure onsets, with live-b repairing.
     - Today `ServerLimits::max_deferred` is a count (4096 per work class, every peer together),
       and a deferred Answer keeps only its request's bytes charged.
     - A deferring handler that holds a reply-sized buffer while it waits is then charged nothing,
       up to 4096 × the reply: 16 GiB at 4 MiB extents.
     - The change: charge a deferred request its expected reply (or bound max_deferred in bytes
       per class), and give each peer a share of a class's allowance.
     - `ServerLimits` is in NodeConfig's layout, so it is a minor.
     - Not live-a's cause, as the code stands: neither Dagda nor Macha binds a deferred handler,
       and the node's ledger is enforced on its server and client. The gap is real for any
       future deferring handler, so it stays planned.
   - Dagda's ask: `NodeConfig::liveness_time`, a TimeSource for liveness probes and call
     watchdogs that defaults to `time`. Today a test stepping a node's clock by more than
     dead_after/3 also gives up its links (client.cpp's await_ping).
3. **Investigate, before 0.11.0's gate:** LevelZero.AChunkLostOnTheWayToTheFarEndEndsItWithinTheBound
   failed once, in hosted CI's x86-64 ASan job on 0.10.2's release commit (run 44842230741).
   - What failed: after the hub severed its data route to B mid-transfer, its data circuits did not
     reach 0 within 20 s, and relay_ends_lost stayed 0.
   - Where it passed: under ASan on a build node, under TSan, and in every other job.
   - 0.10.2 touches no relay or circuit code.
   - Find the mechanism, test first, before calling it timing.
   - Done so far, 2026-11-22:
     - not reproduced in 90 ASan runs on a build node, idle, pinned to one core, and two
       copies sharing one core;
     - on GitHub the transfer ended in about 3 s and the circuit then held for the whole 20 s;
       on a build node the test takes 350 ms.
   - Ruled out by reading the code:
     - a held close is re-looked at every finish_wait (watch_close);
     - sweep() needs a CONTROL route gone, and the test keeps it;
     - a session that closes tells every in-flight and queued chunk "not sent" (abandon), so a
       leg cannot stick at its pipeline limit that way.
   - f2b6608 makes a failure print every node's level 0 counters, its circuits and the
     transfer's outcome. The next failure names the way out that was not taken.
4. **Raise with the maintainer:** `found()` gives each slot its own deadline, counted from when that
   slot's run starts, so with its node gone a founding's outcome takes a deadline per window of 16
   slots.
4. **Dependencies:** read Boost's, OpenSSL's and yaml-cpp's newer releases against Dannan's code
   (the maintainer, 2026-11-20).

**Earlier on 2026-11-20: planning the Cluster Stack's needs; code held.** the maintainer, 2026-11-20, after the duplication and
special-roles audits: "You should start planning now"; on starting code, "Keep holding". The
plan is [`2026-11-20-stack-plan.md`](2026-11-20-stack-plan.md): 31 items in three minors, every
consumer's answer (what it uses today, what it needs, the proof), and the shapes reconciled.
The placements were settled through the coordinating session, which keeps the stack's
placement record. the maintainer's rulings that day: HTTP
and JSON into Dannan, register order by hlc, a DNS responder in Dannan, no automatic removal of
absent nodes, the lower project keeps what two need, Macha injects a logger and the Context (no
holders), no single-node special cases, squash commits onto develop, sanitizers at a feature's
end. AthenaSIP is not part of the group. Next: draw the open shapes (paged transfer and
catch-up first), send each consumer its shapes to confirm, and start code only at the maintainer's word.

**Before 2026-11-20, the one job was the Macha move** (kept below for the record).
the maintainer, 2026-11-18, to Macha, Dannan and Dagda: "work together and focus on the Macha move to
Dagda. Work on nothing else until this is done. Work autonomously until you're ready to do the cutover on live-a
and live-b." the maintainer, 2026-11-19, to Macha: do not begin the cutover; get ready for it. Dannan's part has three
pieces:
- answer Macha Server and Dagda;
- take first any port failure whose cause is in Dannan: prove its mechanism, write the failing test first, then fix;
- start nothing else, 0.10.0 included.

**On a fresh start:** check the claim board and `ListAgents` for peers' messages. Read `../macha/TODO/ACTIVE.md`'s
"Resume here" and `../macha/TODO/2026-11-18-cutover-runbook.md` for where the move stands. Nothing of Dannan's is in
flight. Once the cutover is done, the next Dannan work is 0.9.3 (below); nothing else starts first.

**Where the move stands (2026-11-19, about 03:30Z; no news from Macha or Dagda since).**
- **Macha's port is green.** `../macha` `port/dannan-dagda` at `fe74e9b`, the 0.95.0 release commit, pushed and
  untagged: 545 of 545 on Dannan 0.9.2 with Dagda `053ee6e`. No open failure has a Dannan cause.
- **Dagda 0.3.0**, the read fix included, is in its release gate on node-b and node-c, against Dannan 0.9.2.
- **Macha's remaining steps toward readiness** (its "Resume here"):
  - the Dagda 0.3.0 tag;
  - the deploy tarball;
  - the two-version smoke test (Macha 0.94.2 on Dannan 0.5.3 against 0.95.0 on 0.9.2, on two build Pis);
  - then the maintainer's decisions, each asked with a recommendation.
- **The cutover deploys Dannan 0.9.2** (Macha's decision, 2026-11-18). 0.8.5 has the ledger use-after-free
  (`79dfa2e`) and the unsynced new directory (`ef3b212`).
- **The mixed-version window, answered and checked** (`COMPLETED.md`, 2026-11-19).
  - Dannan 0.5.3 and 0.9.2 form one cluster when live-b keeps the cluster key, custody or not.
  - With a certificate alone they are two islands.
  - Lane and class rules are checked on replies too, at both ends. A refusal ends the session, which bears on five
    store types whose rules differ between Macha 0.94.2 and Dagda.
  - Macha's smoke test covers what node-a's check did not: telemetry contents, advert pushes, and Macha's and
    Dagda's own types.
- **Dagda's own directory-sync gap** (directories it makes before `durable_replace_file`): Dagda's, to fix with its
  move onto `storage::FileSystem`.
- **Custody, as given to Macha.** dannan-custody(1) is the procedure.
  - With the node stopped, run `dannan-custody init --state <state> --credential dannan-root`, piped straight into
    `systemd-creds encrypt --name=dannan-root - /etc/credstore.encrypted/dannan-root`. Init keeps the existing
    `node-id`.
  - The unit gets `LoadCredentialEncrypted=dannan-root`, and `macha.yaml`'s `custody.credential` names
    `dannan-root`.
  - Prove the seal with `systemd-run --wait --pipe -p LoadCredentialEncrypted=dannan-root dannan-custody show ...`.
  - The Pis have no TPM. The off-node backup is three files kept together: the sealed credential, that host's
    `/var/lib/systemd/credential.secret`, and `identity/custody`. The root is never kept in clear (an earlier
    message saying to `tee` it was corrected the same day).
- **`dannan-import-macha --dry-run <state>`** is only a check at this cutover. The live nodes have run Dannan's
  transport since Macha 0.94.0 (Dannan 0.5.3).
  - Expect "already" or "absent" for all three files. An "imported" comes to Dannan.
  - The formats (`DNNMEM01`, `DNNINB01`, `DNNTEL01`) are unchanged from 0.5.3 to 0.9.2.
- **The state files Dannan writes under `state_path`**, as given to Macha:
  - `.dannan.lock`, `node-id`;
  - `membership/known-nodes.bin`, `membership/agreed-nodes.bin`;
  - `telemetry/last-known.bin`, `connectivity/inbound.bin`, `observation/`;
  - `identity/custody`, `identity/counters` (with custody), `identity/revocations` (with certificates);
  - `clock/hlc-ceiling` (since 0.9.0, only without custody).

**Dannan's own state.**
- **0.9.2 is the last release** (`main` and tag at `36d6ded`). `develop` holds only records past it; its last pushed
  commit, `6eaa5fc`, is green in CI (44534666674) and CodeQL. The plan commits after it are local until the maintainer says
  push. Nothing is in flight.
- **Worktrees.** `build/wt-feature` is on `develop`, and `build/wt-main` on `main`. The main checkout sits on the
  merged `feature/advert-push` because `develop` is checked out in the worktree. Every local `feature/*` branch is
  merged into `develop`.
- **Every release from 0.5.6 to 0.9.2 is on the apt repository** (the maintainer published them, 2026-11-19). Each version has
  its 5 packages on arm64 and amd64, all 120 files download, and `InRelease` verifies. the maintainer runs `publish.sh` and the
  upload himself; the rsync never has `--delete` (the maintainer, 2026-11-19; `../tomcully-apt/README` corrected to match).
  Check a publish against the live `dists/trixie/main/binary-<arch>/Packages` and `reprepro list trixie`.
- Lir is on 0.9.2 (361 of 361).

**Gates.**
- The last full suite, on node-a for 0.9.2, ran 942 tests: 941 passed and 1 skipped (skipped when run as root).
- **No macOS build or test without asking the maintainer first** (the maintainer, 2026-11-17, the macOS gate included; 0.9.2's gate was
  his one exception). If he allows one, the setup is in `COMPLETED.md`'s 0.9.x entries: `build/macos` in the main
  checkout, configured against `build/wt-feature`.
- **Sanitizers run locally only for a release or a named bug.**
- **Builds go to the Pi cluster.** The Pis were wiped to bare OS on 2026-11-18 and rebuilt. Dannan has no tree on
  them now: `/root/dannan-vlp` is gone, and its logs are in `build/pi-logs-2026-11-18/`. Check that a tool is
  present before relying on it. node-a, node-b and node-c have 16 GB; node-d has 4 GB, so sanitizer builds
  there use -j1. All four run a 4K-page 48-bit kernel, so ASan, UBSan and TSan work on all of them. node-a has
  ninja since the rebuild. Claim cores first and release them after. Copy the tree with
  `git ls-files -z --cached --others --exclude-standard | tar ... | ssh root@<ip> 'tar -xm -C /root/dannan-vlp'`.
  `-m` matters: without it a file keeps its Mac edit time, and one shipped after a build can be
  older than the objects built from it, so ninja keeps a stale object (2026-11-17: a test object
  with the old `NodeConfig` beside a library with the new one, a segfault).

**After the cutover: 0.9.3, the hybrid clock's ceiling written only for stamps issued** (the maintainer, 2026-11-19: "after
the cutover"). Lir's cluster found every node rewriting the ceiling once a second, idle or not: about 5 to 6 disk
writes a second, each a temp file, an fsync, a rename and a directory fsync. With custody it is `identity/counters`
at the same rate.
- The cause: `keep_ahead()` (`hlc.cpp:131`, from `node.cpp:1504`) raises the ceiling to max(last issued, wall) + 2 s.
  The wall moves 1 s each pass, so every pass writes. Dannan issues no stamps itself, and neither Macha nor Dagda
  uses the clock (no hits in either), so an idle node pays the write to protect stamps nobody issued.
- The fix, as put to the maintainer:
  - the ceiling follows the last stamp issued, never the wall, so an idle node writes nothing;
  - the first stamp after an idle spell raises it once on the write path, counted on Status as `raises_off_loop`;
  - while stamping, it is raised to issued + 30 s when within 2 s, one write each 28 s; a crash restart may then
    start up to 30 s ahead of the wall, half the default `max_ahead`;
  - a clean stop writes the ceiling down to the last stamp, so a planned restart starts at the wall;
  - the directory fsync stays, because the replace is by rename.
  - the constructor's write at start goes too: a node that never stamps never touches the file.
- The design's revision is in `2026-11-15-hlc-design.md`, "Revised (2026-11-19)". The contract text in `hlc.h`
  (`HlcClock`, `keep_ahead`, `reserve`) and `context.h`'s `hlc()` describe the 0.9.2 behaviour and change with the
  code.
- Tests first, from the design: an idle node writes nothing over a stepped minute; the restart rules hold; a crash
  just after a raise starts above every stamp; a clean stop's lowered ceiling is still past every stamp issued.
- The change is invisible to Macha: it keeps the ABI, and the state file format is unchanged. Lir and the maintainer have the
  plan. Lir repins once the maintainer approves the rollout. Its time cost is about a day.

**Waiting on the maintainer:**
- the word for the cutover on live-a and live-b, once Macha and Dagda are ready (asked by Macha).

**Waiting on others:**
- Macha Server: its two-version smoke test, and any failure with a Dannan cause.
- Dagda: 0.3.0's gate and tag; after the move, its disk paths through `storage::FileSystem`. Real Dagda then runs
  above `test::sim::Disk`, which is also step 3's part 6, `SimDisk`.
- Miach: whether it builds the adapter it asked about (answered 2026-11-18; not part of the one job).

**A hang, open:** `ChildProcess.ChildrenStartOnManyThreadsAtOnce` (`fixtures_test.cpp:205`) hung
TSan x86-64 in CI run 44116803795 (`feature/sim-workers`, `eb2342c`) from 15:44 until the job's
60-minute limit; every other job was green. Each child has a 20 s deadline, so something waited
without one; the likely mechanism, unproven, is a child forked from the threaded test process
stuck in TSan's runtime before its exec, with a destructor's wait on it unbounded. First seen;
the test and the reaper have passed TSan in CI since 0.6.0. Reproduce before changing anything.

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
- A release's local gate is release, ASan and TSan on a Pi; the macOS gate only if the maintainer allows it.

**0.10.0, after the move to Dagda and not before** (0.9.0's scope moved these out: no stack project needed them,
`COMPLETED.md`). The wire additions are designed and reviewed first.
1. DTLS and UDP for `dannan::listen`.
2. The peer-witnessed high-water mark. It also covers the hybrid clock's restore without custody (the maintainer's ruling: see
   `2026-11-15-hlc-design.md`, "As built").
3. Loader promotion at the peer. Its design goes to Macha first.
4. Atomic commit across replica sets. Mimir needs it for ruling 22's multi-slot commands now that a change names one
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

   A change naming several names in one slot also goes through it (Mimir and Kvasir agreed); until then `change()`
   takes one name.
5. **Export the fuzz harness** (Kvasir's ask, 2026-11-15; Mimir wants it too), so hosted systems link it rather than
   copy it (the stack's rule 3): `fuzz/fuzz.h`'s Target and Registration, `targets()`, the replay driver, and
   `fuzz_smoke_test.cpp`'s mutate and seeds-then-mutations-then-noise loop as a callable, installed header-only with
   the test fixtures. A hosted system then runs its targets in its own test binary with no clang, which no build Pi
   has.

**Later still** (asked, agreed, deliberately later):
- A sync-free current read (Kvasir): a read-only prepare that is not kept, returning only when every answer matches,
  and otherwise starting again with a kept prepare. Option (b) as first stated is unsafe through its fallback accept.
  It needs Kvasir's simulator run: crashes between the read's promise and a lower-ballot accept.

**Standing, from the maintainer's rulings (2026-11-15):**
- Anything that can use agreement waits for it: no stack project builds its own consensus meanwhile.
- Lugh's TOTP, recovery codes and key rotation use agreement registers on 0.9.0 (relayed by Lugh).
- The `ack_level` names are standard: ephemeral, persisted, decided, replicated (relayed by Kvasir).
- The sim harness is built once, in Dannan (relayed by Kvasir).

**Peers' open threads:**
- Mimir and Kvasir accepted the final `AcceptorStore` and `change_deltas` (2026-11-16), and 0.9.0 is tagged for them.
- Lir is on 0.9.2. Its NodeRecord TSan reports (0.8.3, 0.8.5) are attributed to the ledger double free fixed at
  `79dfa2e`, and reopen only if one appears on a build with the fix (`COMPLETED.md`). It waits for 0.9.3's ceiling
  fix.

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
