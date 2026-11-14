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

Four pieces, in `dannan::sched`, revised after Dagda's, Lugh's and Macha's reviews
(below): the pacer, a per-class gate as the primitive, a pool built on the gates, and
the node's own gates and pool on the Context.

1. **`sched/cpu_pacing.h`**: Dagda's `CpuSample`, `system_cpu_sample()`,
   `background_cpu_scale()` and `CpuPacer` move here byte for byte as Dagda's (names,
   injected sources, the 0.5 target default), so Dagda's swap is a delete and an
   include. Dagda keeps its copy until the slice that first links 0.8 (it is on 0.6
   through Macha's port and moves to 0.7 at slice 4). No behaviour change for Dagda or
   for Macha's credit pacing.

2. **`sched::ClassGate`, the primitive** (Macha's review): one per background class
   per node, the pacing a long-running loop holds and the pool runs its tasks through.
   - It is a `DutyShare` (loader against foreground; speculative against foreground and
     loader) with its contention reading attached: the gate's own (work of a higher
     class queued or running in the node's pool) or'd with the hosted system's
     (`set_contended(fn)`). Macha's is `peer_viewers || idle_for(foreground) < quiet ||
     idle_for(loader) < quiet` (`maintenance.cpp:704-708`).
   - A loop calls `started`, `should_yield` at each operation boundary, and `finished`,
     as Macha's repair does today (`maintenance.cpp:709-720, 791-796`), and reads
     `cpu_scale()` to scale its own credit (`:428-438`). A start check answers how long
     to rest (the cooldown left), never "wait until the higher class is idle": paced,
     never gated.
   - **One gate per class per node**: Macha's loop holding the speculative gate and
     Dagda or Lugh submitting speculative tasks draw on the same `DutyShare`, or each
     would believe it had the whole 5% and the class would take the sum.
   - The contention reading is called on whichever thread asks (a pool thread at each
     pick, a loop at each boundary). It must not block, and must not take a lock a task
     or the loop can hold, or it can deadlock them (Dagda's review).

3. **`sched::WorkerPool`**, built on the gates: a fixed set of supervised threads
   (`work-<n>`) running submitted tasks by `WorkClass`.
   - `submit(WorkContext, task, done) -> Submitted{accepted, why}`: never waits. Each
     class has a bounded queue. A refusal says why: queue full or cap reached (Lugh's
     review; Lugh reports each differently, and a `Budget` refusal is Lugh's own, before
     it submits). Lugh turns a refusal into "try again shortly" (HTTP 503 with
     Retry-After, LDAP busy).
   - **Deadline and cancel honoured at dequeue** (Lugh's review): a task whose
     `WorkContext` deadline has passed, or which is cancelled, while queued is dropped
     unrun, and `done` is posted to the caller's executor saying so (the executor rule).
     A storm of hashes for clients that gave up costs nothing.
   - Order: control, foreground, loader, speculative (laws 1 to 4); FIFO within a class.
   - **Per-class caps and queue bounds, the hosted system's to set** (Lugh's review):
     Lugh sets foreground's cap below the cores (2 on a Pi 5) so its HTTPS and LDAP
     threads keep CPU, and uses the node's pool rather than its own. Dannan fixes the
     four classes, not their sizes.
   - **Reserves** (Dagda's review): a worker is held back for each higher class, as the
     memory reserves are, so loader and speculative tasks, however many are blocked,
     never occupy every worker; control and foreground always find one free.
   - **Blocking** (Dagda's review): a task may block only within its `WorkContext`'s
     declared waits and before its deadline (the wait guard checks). A task that would
     wait on a peer for long should be a deferred reply or a callback, not a held worker.
   - **Pacing**: a loader or speculative task runs through its class's gate, so its
     burst and rest are shared with any loop holding the gate.
   - **CPU pacing**: a background task that used d of CPU earns a rest of
     d x (1/scale - 1) before its class's next task, so above the target background work
     loses time continuously and is never stopped (the scale has a floor). d is the task
     thread's CPU time, not wall time (Dagda's review), so a task that mostly waited on
     I/O earns no rest it did not cost. For byte-bound work this equals Macha's credit
     form in steady state (Macha's review); a loop holding a gate applies the scale
     itself.
   - Injected time and an injected CPU source, so tests step both.
   - Counters per class: queued, running, refused (by reason), dropped at dequeue,
     rested time; on `Diagnostics`.

4. **On the Context**: the node owns the gates and one pool (`Context::workers()`,
   `workers().gate(WorkClass)`), sized by `NodeConfig` (threads, reserves, per-class
   caps and queue bounds, weights, `cpu_target` default 0.5 as Macha's). A hosted
   system may set its classes' caps and bounds before start.
   **Further pools** (Lugh's review): a hosted system may build its own `WorkerPool`
   beside the node's, its threads under the node's supervision (`Context::threads()`,
   counted like any other) and its background tasks through the node's gates, so a
   second pool never adds a second pacer. Lugh will hash on a pool of its own (cap 2 on
   a Pi 5, a small queue, foreground), since on the node's pool a sign-in storm filling
   foreground's queue would get Dagda's writes in the same process refused too.

## Reviews (2026-11-14)

- **Dagda**: yes to the shape and to all of it in 0.8. Its maintenance loop (collector,
  GC, repair, scrub) stays on its own thread through Macha's port, since Macha's pacing
  cases gate that code and it should move once; after that, repair and scrub batches
  become pool tasks. Sooner and larger: slice 2 puts the heads exchange's per-space
  loops (replication, advert, repair turn) on the node pool, since Lir wants hundreds of
  spaces and a thread per loop per space does not scale. Asked for the reserves, the
  rule on blocking, CPU time for d, and the rule on the contention reading.
- **Lugh**: a per-class cap and an immediate refusal meet N6; it needs no waiting
  submit. Asked for the deadline at dequeue, the refusal's reason, and its own cap and
  bound. Then, seeing that the node's foreground queue is shared with Dagda's writes in
  lugh-node, chose a pool of its own for hashing, which needs only that a second pool be
  supported and supervised. Accepted 520648b.
- **Macha**: its maintenance pass cannot be cut into tasks without a rewrite. It owns
  state across passes (three credits, the pull and push lists, the walks' cursors), its
  stages run in a fixed order, and repair yields inside a step. So the gate is the
  primitive and the pool is built on it, with one gate per class per node shared by
  both forms. Moving onto the gate is a small change on Macha's side
  (`WeightedLoaderService` becomes Dannan's per-class gate, the scale comes from
  `cpu_pacing.h`), proved by its pacing cases unchanged.

## Proof

- Dannan's tests, from this design and the laws: order by class, refusal when full,
  caps, reserves, a speculative task paced (never stopped) while foreground runs, a loop
  and pool tasks sharing one gate's slice, the rest earned from a stepped CPU source,
  deadline and cancel at dequeue told on the caller's executor, supervised restart.
- Macha's pacing cases, when Macha's maintenance moves onto the node's gate:
  `test_repair_is_paced_not_stopped_while_higher_classes_stay_busy` and
  `maintenance_trace` stay green unchanged. That move is Macha's port step.
- A baseline run on the nodes (live-a and live-b need the maintainer's word): repair rate and
  foreground latency before and after, at the same load.

## Questions for the maintainer

1. **Scope of 0.8.0**: all four pieces (the pacer moved, the per-class gate, the pool,
   and the node's gates and pool on the Context) in 0.8.0? Recommendation: yes, since
   they change the ABI, 0.8 is open, and all three reviewers want them there.
2. **CPU pacing applied to pool tasks** as a rest after each background task, measured
   in the task's CPU time, while a loop holding a gate keeps scaling its own credit.
   Recommendation: yes; it equals the credit form for byte-bound work, and nothing of
   Macha's changes until its loop moves onto the gate.
3. **The baseline run on the nodes** needs live-a and live-b. Recommendation: after
   Macha's maintenance moves onto Dannan's gate, not before; until then Macha's own
   pacing cases are the gate. Macha agrees.
