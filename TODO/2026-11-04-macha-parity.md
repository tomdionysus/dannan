# Dannan: closing the Macha port list

Status: respec, 2026-11-04 night, being built. Tom's confirmation came relayed by
the Macha session, with these rulings: the Pi measurements and the real IGD run
stay on hold; a Macha case whose subject becomes Dannan's may be retired against a
named `dannan_tests` case, so the seams in item 3 are built only where a
Macha-visible behaviour needs them; two cutovers, transport first. It answers section 5.2 of
`../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md` and the fuller list
the Macha session sent the same night, relayed as Tom's instruction: Macha is
the source of truth for the port, and where Dannan differs from Macha's
behaviour, Dannan changes. Tom's rulings in that document's section 5.3 stand:
wire v1 and v2 and the flag day, adverts in place of `NodeInfo` fields, the
hand-written UPnP, Dannan's own state-file formats.

The Macha session accepted these placements the same night. Every claim was
checked against the code at Dannan `811c448` and Macha's working tree. Each item says what was found, what Dannan will do, and any dispute.

## Rules for this work

- Tests first, from Macha's behaviour; the count only grows.
- A hosted system's names stay out of Dannan's core: no Macha field, path or
  message type in `dannan/`. Macha's bytes appear only in the import tool and
  its tests (item 1).
- A measured number is recorded, never asserted, in the gate: no timing verdict
  in a test.
- A Macha defect found while building this goes to the Macha session, not around
  it: Macha fixes it first and Dannan matches the fixed behaviour (Tom's ruling,
  relayed by the Macha session, 2026-11-04).

## 1. Macha's state files at cutover

**Confirmed.** `membership.cpp:47` refuses `MACHMEM4`, so the node does not
construct on a Macha state directory. `MACHINB1` and `MACHTEL3` are ignored.

**Will do:** a one-shot tool, `dannan-import-macha <state>`, not a reader in the
loaders, so the loaders keep refusing what is not theirs. It converts:

- `membership/known-nodes.bin`, `MACHMEM4` to `DNNMEM01`: members, last heard
  and identity-reset tombstones. Older Macha rosters are refused by name.
- `connectivity/inbound.bin`, `MACHINB1` to `DNNINB01`.
- `telemetry/last-known.bin`, `MACHTEL3` to `DNNTEL01`, using the layout from
  item 7.

It writes durably, keeps the originals beside the new files as `*.macha`, is
safe to rerun, has a `--dry-run`, and refuses a directory whose lock is held.
The node id file is compatible and is left alone.

**Needs from Macha:** a real state directory, under
`../macha/build/macha-fixtures/`, after its tree-native merge. Until then the
tests use bytes from Macha's own encoders (`membership.cpp:18,124`,
`cluster.cpp:66,443`, `telemetry.cpp:753`).

## 2. Journal lines

**Built.** The transport writes through the node's sink. Levels and wording are
Macha's, from `src/cluster/net.cpp`:

| Line | Level | Macha |
|---|---|---|
| BBR unavailable, once per process | warn | `:87` |
| Connection outbound, inbound | info | `:2368`, `:4298` |
| Probe accepted | debug | `:4241` |
| Dial failure, liveness failure | debug | `:2607`, `:2774`, `:2900` |
| Dial request sent, answered | debug | `:2693`, `:2703` |
| Stall notice, repeating each period | debug | `:2536` |
| No-progress cancel | debug | `:2532` |
| Session, handshake and handler errors | debug | `:1711`, `:4386`, `:4433` |
| Stop steps and session reaping | debug | `:3173`, `:4073`-`:4156` |
| Identity-reset propagation failure | debug | `cluster.cpp:1031` |

Stall notices default to Macha's 5 s on CONTROL and 120 s on DATA in `NodeConfig`.
Cases: `Journal.*`, `Node.TheTransportWritesThroughTheNodesSink`. The BBR warning's
path only runs on Linux, so only its wording is tested here; the Debian run (item 8)
exercises it.

The management identity-reset lines (`api/manage_api.cpp:676`, `:682`, `:917`) are
Macha's API and stay in Macha.

## 3. Components not wired in

**Built:**

- `NodeConfig::dial_hook`. Send and admit seams only if the port's case mapping
  finds a Macha-visible behaviour that needs them (Tom's ruling).
- `Context::wait_guard()`: the node's one guard.
- `status::ObservationRecorder` owned by the node, started first, writing
  `observation/observations.jsonl` (64 MiB, one minute, a `start` event), hosted
  gauges through `NodeConfig::observation_gauges`; a zero interval turns it off.
- `fit_memory_limit(NodeConfig&, hosted parts)`: a hosted system calls it before
  building anything sized from its parts, as Macha's `config_base.cpp:26-57` runs
  before anything is built. A node with no hosted parts calls it for itself. The
  node logs the warning. The reserve is named `foreground` where Macha says
  `viewer`.
- `supervise::StartupWatch`: polls the gate on the injected time, logs Macha's line
  at error, then `std::_Exit(1)` or the handler a test gives. The hosted system
  starts it, as Macha's `service/service.cpp:465-509` does, over its own
  `StartupProgress`; the node never starts one itself.

## 4. Context gaps

**Built:**

- `Nodes::refresh_public_connectivity(probe, force_probe, executor, done)`. The
  connectivity loop serves it, since a refresh waits on the network, and the answer
  is posted to the caller's executor; empty when the node is not running.
- A type's `Registration` may name its own `no_progress` and `stall_notice`,
  defaulting to its lane's. Macha's `no_progress_deadline_for` is keyed by type but
  has two values, so this reproduces it.

**Per-plane progress: not built, because Macha has none.** Macha's
`RecoveryProgress` (`src/cluster/local_state.hpp:36-90`) holds plane flags, not
progress; the ticking counter is `startup_progress()`, Dannan's `StartupProgress`.

**Correction, 2026-11-05: `status::Readiness` is not at parity with it,** though this
file and the message to the Macha session first said so. Two differences remain to
close:

- Macha's `complete` is an explicit `mark_complete`, set after recovery returns
  (`service/service.cpp:522`). Dannan's is "every declared plane ready", so a node
  reads as ready earlier than Macha's would.
- Macha's `fail` keeps the first failure (`local_state.hpp:49-57`). Dannan's keeps the
  last (`status/readiness.cpp`).

## 5. Injected time

**Built.** The heartbeat, the ping give-up, the retry pause and the round-trip
sample read the node's `TimeSource`, as does `SupervisedThreads`' "long healthy
run". `TimeSource::wake_within` says how long a timer may sleep: exact on the real
clock, so production keeps one timer per wait, and short on a stepped source. Cases:
`Health.TheHeartbeatFollowsTheInjectedTime`,
`Health.ALivenessBudgetIsSpentInTheInjectedTime`,
`Health.StoppingEndsAProbeWaitingOnStoppedTime`,
`Client.TheNoProgressDeadlineFollowsTheInjectedTime`,
`SupervisedThreads.ALongRunInTheInjectedTimeEarnsAFreshBackoff`.

**Correction to the first reading:** the watchdog already decided by injected time
(`Session::idle_for`); `client.cpp:63` only sets its wake. Its new case pins that.

**Left on the real clock, on purpose:** bounds on real I/O (connect, handshake,
path and reverse-dial timeouts, `reach/connectivity.cpp`'s socket deadline) and the
condition-variable deadline in `sched/retained_memory.cpp`. `Routes::call`'s
deadline and `Time::after` are still real timers too, which a hosted system testing
on stepped time should know.

## 6. A measured gate

**Confirmed:** no number exists anywhere in Dannan.

**Will do:** `dannan_bench`, built with the tests but outside the gate. Two nodes
move 32 MiB on DATA while CONTROL pings are sampled, and it prints MB/s, round
trip percentiles under load and idle, and peak RSS, at 4 KiB and at the default
frame. The same scenario runs on the compose bed through a real NAT and through a
relay. The numbers go to `COMPLETED.md`. A plain case in `dannan_tests` moves
32 MiB and asserts only delivery and integrity.

**Dispute, small:** no threshold until Macha's baseline exists on the Pis, and the
laptop's number is not comparable to it. A comparable number means running
`dannan_bench` on the Pis, which needs Tom's word.

## 7. Telemetry

**Confirmed:** hosted fields from 1024 exist (`status/telemetry.h:63`); nothing
shows Macha's figures fit.

**Will do:** a case round-tripping a hosted layout of every kind Macha's record
uses (the u64 and u32 figures, text, a list), including persistence and a mixed
set; and the proposed field table, from Macha's `telemetry.cpp` ids, written in
this file when built, for Macha to adopt.

**Dispute, small:** the layout is Macha's to own, so the table is a proposal and
Dannan's code has no Macha field in it. The byte identity of `GET
/api/v1/status/nodes` is the Macha facade's gate against the Status golden.
Dannan cannot prove it. `peer_viewers_active` as a traffic-class reading needs
that facade too.

## 8. Platform

**Confirmed:** every sanitizer run so far is on macOS.

**Will do:** ASan, UBSan and TSan on arm64 Debian 13 in Docker, the Pis' OS and
architecture, recorded per release. It also tests whether Dannan needs Boost 1.87
at all: `CMakeLists.txt` asks for it, but if it builds on Debian 13's 1.83 the
build node needs no extra install.

**Waits on Tom:** the real IGD run. It adds and removes a port mapping on the live
gateway, so it is his to allow, along with which machine runs it. Running anything
on macnessa or inverbeg is too.

## 9. Smaller

- BBR warning: item 2.
- Unasserted rows, each gets a case: the once-a-second telemetry cap and "sooner
  on peer observation"; the roster's older-write-skipped rule
  (`membership.cpp:126`); Status never waiting on a probe.
- `by_capacity` against Macha's `capacity_placement_nodes`: **read line by line,
  no difference found** in `water_level`, the quotas, the sample, the domains,
  `within`, the extra-domain step or the fallback order, given Macha's salts and a
  capacity advert. Reading is not proof, so a known-answer case pins the one reference
  set, which is Macha's to produce: the Macha session generates it on macnessa
  from `capacity_placement_nodes`, `rendezvous_nodes` and
  `placement_logical_capacity`, for R = 1 to 3 over 2 to 6 nodes, distinct and
  shared failure domains, unequal capacities and nodes without `hosts_extents`,
  with a README naming the commit and host. Dagda pins the same set. Dannan does
  not compile `placement.cpp` itself.
- **Checked, 2026-11-04:** Macha's reference set
  (`../macha/build/macha-fixtures/placement/placement-vectors.json`, macnessa,
  `5776392`, 113-bit mantissa) was run once against Dannan 0.4.1's `by_capacity`,
  `rendezvous` and `logical_capacity` from a scratch program, under Macha's salts:
  2,704 orders and 26 capacities, no differences. Still to do: pin it as a case
  in `dannan_tests`.
- **Long double:** both trees score fallbacks in `long double`, whose mantissa
  differs by platform: 113 bits on Linux arm64, 64 on this laptop (x86_64), 53 on
  Apple Silicon. Macha's laptop run matched its reference on every entry, so these
  inputs agree, but that shows these inputs only: very close scores could still
  order differently between platforms. The Linux file stays the reference, and
  the case also runs in the Debian image.

## Records

Done with this respec: the inventory rows citing the deleted `Interop.*` and
`Channel.ProtocolVersionIsTwentyThree` tests, and the stale sources paragraph,
node names and release numbers in `ACTIVE.md`.

## Order

1. Injected time (5): the later tests depend on it.
2. Journal lines and the sink (2, 9).
3. Wiring and seams (3), then Context gaps (4).
4. Unasserted rows and the placement known answers (9).
5. Telemetry case and table (7), then the import tool (1).
6. Measured gate (6), Linux sanitizers and the Boost question (8).

Then a release, with its sanitizer runs, at Tom's word.
