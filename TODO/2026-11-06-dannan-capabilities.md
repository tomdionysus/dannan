# Dannan's capability report, for the port (2026-11-06)

For the Macha session, which judges this and builds the port (Tom's rulings, relayed 2026-11-06). Written
from the tree at the head of `develop`. Macha's baseline: `9a08846`, 0.93.4.

## The answer

**I cannot say Yes.** Whether Macha works just as well on Dannan can only be shown by Macha running on it,
and that is your test on the nodes. What I can say: I know of no Dannan work left that I can do without the
port. That is not a claim that nothing is left. Today's tests found real defects in code earlier documents
called proven (listed below), so expect the port to find more. Fix them as Tom ruled: the port pauses and I
fix them.

## Gates, at tag 0.5.2

Each gate ran once (Tom, 2026-11-06: "Test once and be done with it."), all at the tag's code, with the host quiet (load average
under 10 at the start). Every commit was reviewed by the Macha session. The case that needs a non-root user skips as root, so
Debian (a root container) and arm64 count one fewer.

| Gate | Result |
|---|---|
| macOS, plain | 639 of 639 |
| macOS, ASan with UBSan | 639 of 639, no reports |
| macOS, TSan | 639 of 639, no warnings |
| Debian 13, g++ 14, plain, ASan with UBSan, TSan | 640 each, no reports, one skipped as root |
| Compose bed, real NATs | 3 of 3; 418.8, 225.4 and 151.5 MB/s |
| arm64 (inverbeg, Pi 5, g++ 14), plain and ASan with UBSan | 640 each, no reports, one skipped as root; run at nice 19 with Macha up |

**Found by arm64 at 0.5.1 and fixed in 0.5.2: a stop held by a retiring session.** When two sessions to one peer settled into
one, the loser left the route tables under the client's lock and joined its retiring list only after the lock was let go. A
`Client::stop` between the two found it in neither, so a call already on it waited out its deadline (30 s in
`Node.CancellingOutboundCallsReleasesAWaitingWorker`). A loser that reaches retirement after stop is now closed, and an
admission holds an operation across its table change, ended by a guard, so stop waits for those closes.

**Also from arm64 at 0.5.1, test-side:** `Client.TheNoProgressDeadlineFollowsTheInjectedTime` stepped time before the call's
own write was recorded as progress (a write's progress is now stamped before its bytes are counted, and the case waits on the
count); and `Supervisor.ADirectoryThatCannotBeListedIsWarnedOf` cannot lock a directory against root, so it skips as root.

**A correction to 0.5.1's record:** its macOS plain line came from a test binary not rebuilt after the last two commits. The
tag's code passed in five fresh builds (macOS ASan and TSan, Debian plain, ASan and TSan); the macOS plain gate now builds first.

**Closed since 0.5.0: relay bulk transfers under Debian TSan.** The relay now numbers chunks (pipelined, reordered at the far
end), paces by credit, carries DATA circuits on the DATA lane, and bounds every wait for a missing chunk; wire v1 section 6.
All relay cases pass under Debian TSan on a quiet host. At a host load of 46 on 12 cores four of them ran out their fixed time
limits with no sanitizer report, and all 17 passed with the same binary at load 19. That is a test's time limit under starvation,
recorded here, not a correctness failure.

**Found and fixed since 0.5.0: a lost wakeup in `Node`.** A forced `refresh_public_connectivity` asked for between the
connectivity loop's test and its wait was not seen until the next heartbeat (one hour in the test that caught it). The same race
delayed telemetry demand by up to an interval and the roster write by up to a second. Every waker now passes through the wait's
mutex (`Node::wake_loops`). Macha has the bounded telemetry form (`cluster.cpp:958`), recorded by the Macha session for after the
freeze.

**Open: a handshake timeout on macOS TSan. Not reproduced at will, cause not found, not called environmental.**
`LevelZero.ANodeIsObservedFromAFixedLocalPort` has now failed in 3 of about 14 full macOS TSan runs, the third at a host load of 46.
It passed 40 alone under TSan, 60 plain, 119 under CPU hogs and 10,635 under parallel copies. The failure text says `timed out at
the handshake from local port N`: TCP connected and then no answer came, with neither end closing. The code that reads the answer
was read and shows no fault. Whether it can happen on the nodes is unknown. Dial and accept share this handshake code, so this one
is on Macha's path until shown otherwise.

**Recorded, not changed:** a relay chunk must fit one negotiated frame; the smallest frame is 4 KiB.

**A test-only risk:** `test::free_port` can collide across processes ("cannot bind the punch port: Address already in use", 2 in
about 10,600 runs under parallel copies). A gate could fail this way if another session runs tests at the same moment. Left as it is.

**One suppression added**, `shared_target_executor`'s destructor: Asio's count is a release decrement and an
acquire fence (read in the image's `detail/atomic_count.hpp`), and g++'s TSan does not model fences.

## Capabilities and evidence

The function inventory, `TODO/2026-11-03-macha-function-inventory.md`, is the row-by-row evidence, with the
named case for each; no row is left partial. Two rows are carried by Dannan and owned above it (below).

- **Transport:** lanes, frame classes, limits (256, 128 MiB, reserve, 512 pinned), the handshake and AEAD, live
  sessions ending on tamper and replay, a stopped peer never blocking a sender, stalls and deadlines on both lanes.
- **Health:** Macha's probe behaviour and words: one pass at a time, a third of the budget per attempt, DATA errors
  silent, a stalled DATA ping retried, a lost CONTROL route dialled again.
- **Membership, placement, telemetry, supervisor, readiness:** see the inventory. `Placement.MatchesMachasReferenceEverywhere`
  and `ClusterKey.DerivesWhatMachaDerivesFromTheSameFile` are checked against Macha's own output.
- **Journal:** of 83 lines, 71 are Dannan's, in Macha's words, each with a named case; 12 are yours. Table: first
  cutover, section 5.
- **Limits:** `sched::ClassWaits`, shared with Dagda. Budget table in the parity file.
- **Importer:** `dannan-import-macha`, with rollback byte for byte.
- **Relay and punch (not on Macha's path):** Macha's nodes dial each other. Flow control is new today.

## What changed today that you should re-read

Each was a difference from Macha, or a defect, found today:

- `frame_type_name` said `read_ahead`; it now says `read-ahead`. This also feeds your Status (`api/status_api.cpp:989`).
  **Dagda should rerun its tests.**
- A call's dial failure is no longer journalled; only lane maintenance says so, as yours does.
- A stalled call's failure text is now the journal's text.
- An empty advertised host and failure domain now fall back as `cluster.cpp:38` does.
- A UPnP removal when the gateway cannot be found keeps the mapping owned, to try again.
- A fault sink kept past a subsystem's stop was a use-after-free; it is ignored now.
- Overlapping health passes could redial a route a probe had just closed; found only because I added the redial.
- A call's deadline and `Time::after` now follow injected time.
- A relayed session's pings wait behind its own circuit, so the far end's movement counts as proof of life, and gives the whole budget again. Direct sessions are unaffected.
- A DATA circuit's bytes take the DATA lane at each hop, as wire v1 section 6 says; they had all been on CONTROL, in front of its pings.
- Since 0.5.0: relay chunks are numbered and credited, and every wait for a missing chunk is bounded (wire v1 section 6).
- Since 0.5.0: a session runs a copy of its handler, so a reply that closes the session cannot destroy the running one.
- Since 0.5.0: the node's loops are woken under their wait's mutex, so an asked-for refresh is never lost until the heartbeat.
- Since 0.5.1: a session that loses its route as the client stops is closed, and stop waits for it, so stop is never held by a call's deadline.

## What the port needs from Dannan, ready to use

| Need | Where |
|---|---|
| `metadata_notice` read ahead of every queue (F10) | `Registration::on_session`; its handler must not wait |
| "a macha plugin" in the journal | `SupervisorOptions::system_name = "macha"` |
| `hosts_extents=` in the inbound line | `NodeConfig::inbound_journal_fields` |
| Every call's deadline (ruling A) | required by `Routes::call`; stall settings default to 0, so state yours |
| Per-call and per-type watches | `net::CallWatch`, `Registration::no_progress`, `stall_notice` |
| Stop and lifecycle (F9) | `Node::request_stop`, `Readiness::hold_until_marked`, `mark_complete` |
| Hosted telemetry | ids 1024 to 1045; message numbers 512 plus the current one |
| Retired message numbers | `Routes::retire` for 549, 550, 560, 636 |
| Memory fit (F18) | `fit_memory_limit`, or keep your `validate` and leave the limit 0 |
| UPnP on (F4) | build a gateway into `NodeConfig::gateway`, or mapping says `support_not_built` |

Yours by finding: F7 (derive topology from `Nodes::watch`), F8 (keep your activity clocks), F9's construction
order, F11 (`version`), F17 (the importer's locks), F21 (replicas advert reader), F22 (plugins rebuild), the 12
journal lines, and the two carried rows: failing closed on `metadata_write_replicas_required`, and pacing repair
by peer viewers.

## What I could not show without a running port

Check these on the nodes:

1. **Everything end to end:** Status bytes, widened goldens, client contracts, and Macha's own suite on Dannan.
2. **Performance against the baselines.** `TODO/2026-11-06-pi-baselines.md` is whole-system, through the mount and
   the API. I have no comparable figure. Mine are loopback, on Pis under load, before today's work: 282 to 347 MB/s
   at the default frame, idle ping p50 under 0.2 ms. No measurement has crossed two real machines.
3. **Reliability over time.** No soak has run on Dannan on real hardware. Liveness under real load is untested; F16's
   budgets now match Macha's 30 s, on reading only.
4. **A real Macha state imported and started.** The importer has run in tests, not as a full node start on a real
   state with Macha on top; nor has Macha opened a rolled-back state.
5. **Real UPnP through Macha.** Tested with a fake gateway and a test IGD; the one real run is a small separate process.
6. **BBR chosen on the Pis.** Asserted only as "bbr or a warning"; the Debian container's kernel decides.
7. **TSan on arm64.** None is possible there with g++ 14 (47-bit VMA); a clang attempt is untried.
8. **macnessa's heap corruption.** It is in Macha's code at 0.93.4, not Dannan's. Dannan's transport is modelled on
   it but shares no code, and I have not looked for a shared cause. Nothing here shows it absent from Dannan.

## Done at Tom's word, after this was written

Tag 0.5.0 and the push. The real gateway run on macnessa passed: a mapping made, read back, removed and confirmed gone.

Tag 0.5.1 and the push. The arm64 rerun at 0.5.1 found the three items fixed in 0.5.2 (Gates, above).

Tag 0.5.2 and the push, with arm64 among its gates.
