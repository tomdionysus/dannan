# A deterministic simulation harness (0.9.0)

Asked jointly by Kvasir and Mimir (2026-11-15), and independently by Macha, Dagda and Miach;
the maintainer, 2026-11-15, to Dannan directly: built once, in Dannan's test fixtures, within 0.9.0,
beside agreement. For review by Kvasir and Mimir before it is built. Its seed is agreement's
own simulator (`dannan/test/agree_caspaxos_test.cpp`, `RacingProposersNeverBothWin`),
which has already found one ambiguity (a proposer whose commit's answers were lost reading
its own write as taken).

## What it is

A world of simulated nodes on one thread, with no real time, sockets or disks. The clock,
the transport between nodes and each node's durable store are injected, every choice is
drawn from one seed, and a seed reproduces a run exactly. Above the seam runs real code:
Dannan's agreement (the proposer, the acceptor rules, the wire messages, the runtime) and
the hosted system's `AcceptorStore` and whatever it builds on it (Mimir's commands, Kvasir's
commit path), never a model of them.

## The seams

- **Time.** A `SimTime` (`contract::TimeSource`) that moves only when the world steps it.
  Every timer above the seam waits on the injected time; agreement's backoff and deadline
  move off Asio's real-time timers onto it (a change to `agree/agreement.cpp`, made with
  the harness). Clock jumps and skew per node, on the hybrid clock once it exists.
- **Executor.** One `io_context`, run by the world step by step (`poll_one`), so every
  handler's order is the world's choice.
- **Transport.** `SimNetwork` implements `Agreement::Call` (and later Routes' calls): each
  message is an event in the world's queue, delivered, delayed, reordered, duplicated or
  dropped as the seed says. Reachability is a directed matrix, so a partition may be
  partial (A reaches B, B does not reach A), and per message type (one answers, another
  does not).
- **Storage.** `SimDisk` behind a hosted store: writes land in a volatile layer, a sync
  makes them durable, a crash drops the volatile layer. Faults: a sync that fails; a sync
  that reports success and loses the bytes; a torn write (a prefix of a record durable);
  a node killed between any two durable writes.

## Faults

Each scriptable, each drawn from the seed: a process killed between any two durable
writes; a node gone for ever mid-operation; a restart without its state (a new
incarnation, which must catch up before it votes); partitions, partial ones included;
slow nodes and slow links; reordered, delayed and duplicated messages; failed, lying and
torn syncs; clock jumps and skew.

## History and checkers

Every client operation's invocation and response (or its absence) is recorded with the
world's time and the node it ran on. Checkers live with the harness, in one place:

- **Linearizability per key** (Mimir's): a Wing-Gong search over each key's history,
  bounded by the history's size, with a per-key model the owner supplies (a register,
  SET NX, a list's pops). An operation with no response may have taken effect or not; the
  checker tries both.
- **Serializability of transactions** (Kvasir's): the committed transactions checked
  against a sequential model the owner supplies, by searching for a serial order that
  explains every read.
- **Agreement's own invariants**, checked on every step: at most one value chosen per
  ballot; a value once chosen is the base of every later choice; `changed_at` monotonic per
  name; no acceptor promises below a promise it holds.
- **Mimir's reducer case**: acceptors at one highest ballot holding different merged
  states; every reduction checked to advance the base only (each applied operation ordered
  after the base's commit, Mimir's contract), and a reducer that rolls the base back
  refused.

## The checkers checked

A verifier that has never refused anything is unverified (Miach). Each checker runs in the
test binary against protocols broken on purpose, and must refuse each with a counterexample
(the seed and the shortest history it found):
- an acceptor that votes after losing its state;
- a proposer that skips prepare;
- an acceptor that accepts below its promise;
- a reducer that rolls the base back;
- a store whose sync lies, with a hosted system that trusts it.

## Cost

A fixed set of seeds (some hundreds, each bounded in steps) runs in the ordinary test
binary on every gate; `DANNAN_SIM_SEEDS` and `DANNAN_SIM_STEPS` set a long random run by
hand on a build server. A failing seed prints itself and its shortest history, and is added
to the fixed set.

## Where it lives

`dannan/test/fixtures/sim/` (header-only, as the other fixtures are), linkable by a hosted
system's tests through the `dannan_test_fixtures` target: `world.h` (the world, its seed,
its queue), `time.h`, `network.h`, `disk.h`, `history.h`, `linearizable.h`,
`serializable.h`, and agreement's adaptor (`agreement.h`: a simulated node's `Agreement`
over the world's network and time).

## Order

1. The world (its own generator), `SimTime`, `SimNetwork` with Routes and simulated
   membership and placement, `SimDisk` on the stack's durable-file contract, the simulated
   worker pool, client actors, the history with faults and resolutions; agreement's timers
   onto the injected time; agreement under every fault, correlated kills included, with its
   own invariants checked; the broken protocols refused. The existing simulator moved onto
   it.
2. The linearizability checker, with owner-supplied models and the per-level durability
   rules; Mimir's reducer case.
3. The transaction checker by isolation level (dependency cycles), with Kvasir's model and
   order options.
4. Clock skew on the hybrid clock, once it is built.

## Kvasir's review, taken

- **Isolation levels, not only serial order.** Snapshot isolation (Kvasir's REPEATABLE
  READ) permits write skew, so a serial-order search would refuse correct histories. The
  transaction checker takes the level: serializable, snapshot (no G0, G1 or G-single
  cycle; G2 allowed) and read committed, by cycle detection over Adya's dependency graph.
  The world's clients write values unique per write, so every read names its writer and
  the graph is read off the history, not searched for. The serial-order search stays as
  the plain reading of SERIALIZABLE on small histories. An order option: real time,
  session order (a session sees its own earlier commits), or none.
- **A resolution event.** An operation records its invocation, its answer, and an
  optional later resolution: at `ephemeral` and `persisted` a commit is answered before the
  cluster decides it, and later decided or refused. An answered-and-unresolved commit is
  treated as a missing response: it may or may not have taken effect. Mimir's blocking
  commands use the same shape (an invocation, then its answer when it comes).
- **Faults in the history; durability judged against them.** Every fault (a node killed,
  a sync that lied, a disk torn, when) is an event in the history, and the checker has a
  rule per `ack_level`: a `replicated` write survives any loss leaving a majority's disks;
  a `decided` one may be lost only if a majority stopped together before syncing; a
  `persisted` one only with its node's disk; an `ephemeral` one with its node. A loss the
  faults do not excuse is a defect; one they excuse is not.
- **Correlated faults.** "These nodes all killed at this step" is one fault the seed draws,
  a whole majority included: the Pi cluster's real failure is several nodes losing power
  at once under load.
- **Real Dagda above the seam.** `SimDisk` implements the durable-file contract the stack
  writes through, so Dagda's node-local log runs unmodified above it and Kvasir's store on
  it is real, not a fake. Which contract that is (Dannan's `identity/durable_file.h`, or
  Dagda's own file layer) is Dagda's to say; asked.
- **Routes, roster and placement in phase 1.** Simulated membership (a node seen leaving by
  some and not others, rosters that differ briefly), `Routes::call` through the world, and
  placement over each node's own roster come with the first phase, not later. The broken
  protocols gain "two proposers with different rosters naming different replica sets",
  aimed at the reconfiguration register.
- **The worker pool in the world.** A simulated `WorkerPool` whose tasks are events in the
  world's queue, so nothing a hosted system runs on it escapes the seed.
- **Seeds that replay anywhere.** The harness's own generator and distributions, fixed in
  its header, never the standard library's (libc++ and libstdc++ differ): a seed found on a
  Pi replays on the Mac.
- **Clients as actors.** A client is a coroutine-like actor taking several steps (a
  transaction is statements with other work between), so the world interleaves at
  statement granularity.
- **Header-only, kept light.** The checkers in their own headers, included only by the
  tests that check (a build Pi has 3 GB).

## Open for review

1. Kvasir, Mimir: is a model the owner supplies (a small C++ type: apply an operation,
   compare a result) the right seam for the checkers, or do you want to supply the whole
   checker and use only the history?
2. Kvasir: is a search for a serial order enough for your sizes, or do you need Elle-style
   cycle detection over dependency graphs?
3. Both: is header-only right, or would you rather link a static library?
4. Mimir: does a recorded "response or none" per operation cover blocking commands (a pop
   waiting for an element), or do they need their own event kind?
