# Dannan's capability report, for the port (2026-11-06)

For the Macha session, which judges this and builds the port (Tom's rulings, relayed 2026-11-06). Written
from the tree at the head of `develop`. Macha's baseline: `9a08846`, 0.93.4.

## The answer

**I cannot say Yes.** Whether Macha works just as well on Dannan can only be shown by Macha running on it,
and that is your test on the nodes. What I can say: I know of no Dannan work left that I can do without the
port. That is not a claim that nothing is left. Today's tests found real defects in code earlier documents
called proven (listed below), so expect the port to find more. Fix them as Tom ruled: the port pauses and I
fix them.

## Gates, at this commit

| Gate | Result |
|---|---|
| macOS, plain | 622 of 622 (1 BBR case skipped) |
| macOS, ASan with UBSan | 622 of 622, no reports |
| macOS, TSan | 622 of 622 in 3 of 4 full runs, no warnings. **1 run failed** (below) |
| Debian 13, g++ 14, plain, ASan with UBSan, TSan | 624 of 624 each, no reports (one suppression, below) |
| Debian TSan, relay transfers under 4 CPU hogs | 14 of 14, no warnings; stopped at its time limit, 14 of 30 planned |
| Compose bed, real NATs, relay measurement | 5 of 5 (was 2 of 8), 103 to 141 MB/s |
| Compose bed, the two other cases | pass |
| arm64 (inverbeg) | **Not rerun.** Last at `415c6c4`, 562 of 562 plain and ASan, before all of today's work |

**One failure not explained.** `LevelZero.ANodeIsObservedFromAFixedLocalPort` timed out at its 10 s handshake
bound in one full macOS TSan run. It then passed 40 of 40 alone under TSan, 60 of 60 plain, and 119 of 119
under 12 CPU hogs, and 3 further full runs. Cause not found, so a stuck handshake is not excluded.

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
5. **Real UPnP (R6).** Only a fake gateway and a test IGD.
6. **BBR chosen on the Pis.** Asserted only as "bbr or a warning"; the Debian container's kernel decides.
7. **arm64 at this commit.** No TSan is possible there with g++ 14 (47-bit VMA); a clang attempt is untried.
8. **macnessa's heap corruption.** It is in Macha's code at 0.93.4, not Dannan's. Dannan's transport is modelled on
   it but shares no code, and I have not looked for a shared cause. Nothing here shows it absent from Dannan.

## Waiting on Tom

R5, a tag and a push (Dagda's `feature/class-waits` waits on it). R6, the real gateway. An arm64 rerun on a Pi, at his word.
