# P4: a class-aware worker pool with CPU pacing (design, 2026-11-14)

the maintainer's ruling (2026-11-13): "move CPU pacing into a class-aware worker pool in Dannan.
Macha's proof is its pacing cases plus a baseline run on the nodes." This note is the
shape proposed for it, for the maintainer's word and for review by Dagda, Lugh and Macha, before
any code. Nothing here is built.

## What exists

- **Dagda** `store/cpu_pacing.h` (from Macha's `maintenance.cpp:247`): `CpuSample`
  (process and own-thread CPU seconds), `background_cpu_scale(process, own, wall,
  target)` (1 at or under the target, else target over the load of everything but the
  pacing thread), `CpuPacer` (differences successive samples). Macha multiplies its
  background byte credit by the scale each maintenance pass (`maintenance.cpp:428-438`),
  so above the target background work loses budget continuously rather than stopping.
- **Dannan** `sched::DutyShare` (`sched/duty_share.h`): Macha's `WeightedLoaderService`,
  a duty cycle by weight (95:5) against contending higher-class work, paced and never
  stopped, yielding at operation boundaries. Macha's repair uses it as `repair_share`
  (`maintenance.cpp:704-720, 795`).
- **Dannan** `sched::ClassWaits` (`sched/class_waits.h`): per-class waiting, under which
  retained memory admits by class.
- **Dannan** `Budget` (`context.h`) admits memory only, not CPU.
- **Lugh N6**: Argon2id hashing must not starve the node. Answered so far: foreground
  work, bounded by reserving each hash's memory through `Budget`, on Lugh's own threads.
  The gap is CPU: a storm of hashes within the memory budget still takes every core.

## Proposal

Three pieces, in `dannan::sched`, each usable alone:

1. **`sched/cpu_pacing.h`**: Dagda's `CpuSample`, `system_cpu_sample()`,
   `background_cpu_scale()` and `CpuPacer` move here unchanged (the time source
   injected, the CPU source injected, as Dagda's). Dagda then deletes its copy and
   includes Dannan's. No behaviour change for Dagda or for Macha's credit pacing.

2. **`sched::WorkerPool`**: a fixed set of supervised threads (`work-<n>`) running
   submitted tasks by `WorkClass`:
   - `submit(WorkContext, task) -> Submitted{accepted, why}`: never waits. Each class
     has a bounded queue; a full queue refuses at once ("try later"), as Lugh's hashing
     wants. A task past its context's deadline or cancelled before it starts is not run
     and its completion is told why.
   - Order: control, foreground, loader, speculative (laws 1 to 4); FIFO within a class.
   - Per-class concurrency caps (`max_running[class]`): CPU admission for Lugh. Hashing
     is foreground with a cap, so a storm queues or is refused, never takes every core.
   - **Pacing of background classes**: speculative work runs under a `DutyShare` against
     foreground and loader; loader work under its own against foreground. "Contended"
     is the pool's own reading (a higher class queued or running) or'd with a reading
     the hosted system gives (`set_contended(class, fn)`), which is where Macha's
     `peer_viewers` and store `idle_for` readings plug in.
   - **CPU pacing**: a `CpuPacer` sampled each tick gives a scale for background
     classes; a background task that ran d earns a rest of d x (1/scale - 1) before the
     next background task starts, so above the target background work loses time
     continuously, as Macha's credit does, and is never stopped (scale has a floor).
   - Everything is injected time (`TimeSource`), so tests step it.
   - Counters: queued, running, refused, rested time, per class; on `Diagnostics`.

3. **On the Context**: the node owns one pool (`Context::workers()`), sized by
   `NodeConfig` (threads, caps, weights, `cpu_target` default 0.5 as Macha's). A
   hosted system may also build its own `WorkerPool`.

## Proof

- Dannan's tests, from this design and the laws: order by class, refusal when full,
  caps, a speculative task paced (never stopped) while foreground runs, the rest earned
  from a stepped CPU source, deadline and cancel before start, supervised restart.
- Macha's pacing cases, when Macha's maintenance moves onto the pool:
  `test_repair_is_paced_not_stopped_while_higher_classes_stay_busy` and
  `maintenance_trace` stay green unchanged. That move is Macha's port step.
- A baseline run on the nodes (live-a and live-b need the maintainer's word): repair rate and
  foreground latency before and after, at the same load.

## Questions for the maintainer

1. **Scope of 0.8.0**: pieces 1 and 2 (the pacer moved, and the pool), with 3 (a node
   pool on the Context) in the same minor? Recommendation: all three in 0.8.0, since
   they change the ABI and 0.8 is open.
2. **CPU pacing applied to tasks** as a rest after each background task (above), rather
   than only as Macha's byte-credit multiplier. Recommendation: yes. The credit form
   stays available to Dagda through piece 1, so nothing of Macha's changes until its
   maintenance moves.
3. **The baseline run on the nodes** needs live-a and live-b. Recommendation: after
   Macha's maintenance moves onto the pool, not before; until then Macha's own pacing
   cases are the gate.
