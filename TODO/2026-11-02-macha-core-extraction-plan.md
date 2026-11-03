# Macha Core Extraction Plan

Rendered on 2026-11-03 from Tom's Claude Doc of 2026-11-02 (revision 9).
The doc is the master until the split; this copy exists so a session in
Macha's tree can read it. Edits go to the doc, then this file is re-made.

## Purpose

Extract the distributed-OS kernel that Macha already contains into a core
that other hosted systems can run on, and refactor Macha to be its first
hosted system, with no change to what a Macha user or operator sees.

The invariant is checkable, not aspirational: the correctness gates in
`VALIDATION.md`, the crash matrix in `docs/durability.md`, the HTTP API,
the FUSE behaviour and the config schema are the definition of
"unchanged". A seam moves only when those pass before and after, on a
real multi-node cluster under chaos.

The systems that will later run on the core are peers, not layers: a
KV/database, an event system, a filesystem, a compute platform, and the
Lugh directory. None is built on another. Only the kernel is common, and
the kernel is larger than a storage engine: scheduling, memory,
durability, identity, membership and placement, with its laws. This plan
does not design those systems; it makes the door they come in through, by
moving Macha's own filesystem out through it.

## What the core is

The membership test: a thing is core only if every hosted system needs it
identically, and no hosted system needs it in its own shape. By that test
the commit DAG's machinery is core and its merge function is not; the
object store is core and "extent" is not.

Core, as it stands in `src/` today:

| Concept | Where it lives now | Status |
|---|---|---|
| CONTROL / DATA lanes, typed RPC routes, priority transport | `cluster/net.hpp`, `cluster/message_routes.hpp` | Generic; `MessageType` enum is closed |
| Work classes, memory reserves, the four laws | `cluster/frame_type.hpp`, `contract/work.hpp`, `cluster/activity_clocks.hpp` | Generic |
| Membership, liveness, failure domains, identity reset | `cluster/membership.hpp`, `types.hpp` | Generic; `hosts_extents` on `NodeInfo` is FS-specific |
| Capacity placement, deterministic fallback, replica ordering | `cluster/placement.hpp` | Generic, pure functions |
| Content-addressed replicated object store, DATA and CONTROL | `cluster/distributed_store.hpp`, `storage/local_store.hpp`, `contract/object_store.hpp` | Generic; hydration and read-ahead promotion are media-shaped |
| Durability domains, tickets, `syncfs` barrier | `storage/durability_domain.hpp` | Generic |
| Retention claims, observed-remove GC | `contract/claim_store.hpp`, `storage/retention.hpp`, `storage/unreferenced_since.hpp` | Generic; "referenced" is computed by a catalogue-aware horizon builder |
| Merkle trie, trie diff, anti-entropy | `ledger/object_trie.hpp`, `ledger/trie_diff.hpp` | Generic, strongest piece |
| Commit DAG: immutable commits, acceptance at write floor, head sets, certificates | `metadata/metadata.hpp`, `metadata/metadata_manager.hpp` | Machinery generic; state type and merge are the FS's |
| Claimed collection with epochs (soft lease, fencing) | `torrent/torrent_request.hpp` | Generic pattern, torrent-only code |
| Bounded work: budgets, pages, retry, memory leases, sealed journal, `Published<T>` | `contract/walk.hpp`, `retry_policy.hpp`, `retained_memory.hpp`, `storage/sealed_journal.hpp` | Generic |
| Plugin supervisor, fault sink, restart policy | `subsystem/subsystem_supervisor.hpp` | Generic; the context it hands out is FS-shaped |
| Fresh-genesis layout contract, refuse-don't-adopt | `storage/`, `config.hpp` | Generic |

Not core, and must never become core: a KV API, a merge rule, a
filesystem namespace, a topic, a task definition, a global clock, a lock,
a transaction.

The API the core offers a hosted system keeps the distribution visible.
Every call carries a work class, a deadline and a principal with scope.
Every call returns evidence (which nodes, which durability tier, which
ticket), never a boolean. The nouns are Node, Object, Claim, Lease, Head,
Channel, Ticket, Budget and Fault. The core stores, replicates and reports
heads; it calls the owner's merge on divergence and never merges itself.

## Where the filesystem is compiled into the kernel

Six places. Each is a seam in the sequence below.

| Entanglement | File | Effect |
|---|---|---|
| `MessageType` is a closed `enum class : uint16_t` with `get_torrent_jobs`, `torrent_intent`, `*_metadata` members | `cluster/net.hpp:28` | A hosted system cannot own RPC types without a core edit |
| `HttpServer` takes one `std::function` handler; route classes are a hard-coded prefix list | `http/http.hpp:182`, `service/service.cpp:73` | No HTTP route registration for a hosted system |
| `SubsystemRegistry` has typed slots `publish_torrent`, `publish_fuse` | `subsystem/subsystem_registry.hpp` | No generic capability publication |
| `MetadataSnapshot` carries `FsEntry` entries, `catalogue_root`, `namespace_root`, `torrent_requests`, `node_status` | `metadata/metadata.hpp:142` | The only durable mutable state is the FS's, and it is the on-disk and wire format |
| `metadata_merge.cpp` knows paths and renames | `metadata/metadata_merge.cpp` | The FS merge runs inside the kernel |
| `NodeHorizonBuilder` includes `catalogue/catalogue.hpp` | `ledger/node_horizon_builder.hpp` | GC reachability is catalogue-aware |

File:line references are from the `develop` tree at 0.91.0 as cloned on
2026-11-02; re-check before each seam.

## Harness first

No seam moves until a multi-node cluster under continuous chaos asserts
the gates every night. The cluster has been at one live node since
2026-10-14 (`TODO/ACTIVE.md`), so head reconciliation and repair-by-diff,
the machinery being moved, are currently unprovable.

- Twelve small nodes across at least two providers. Three to four with
  public addresses; the rest `inbound_capable: false` behind NAT gateways,
  so the dial-out path is exercised.
- A cloud-level chaos monkey with four moves: hard terminate (a brownout
  to the node), network partition by security group (a split cohort),
  volume detach mid-write (a torn SD card), and clock skew. A node that
  does not return after a stop is a law-4 finding by itself.
- One persistent cluster, not per-seam spin-ups: the bugs worth finding
  appear after the fortieth kill, not the first.
- Nightly assertion = every gate in `VALIDATION.md`, the
  `docs/durability.md` crash matrix, plus three new gates: divergent
  heads reconcile with no lost commit; repair-by-diff converges after
  partition heal; a node with a torn volume rejoins without hand repair.
- Local gates in `VALIDATION.md` remain the per-commit check; the chaos
  cluster is the gate for merging a seam.

Deliverables: a Terraform module for the cluster, a scenario runner for
the monkey, a gate report posted as a scheduled task. These are the first
thing every later hosted system needs, so they are not a tax on the
extraction.

## Seam sequence

Strangler order: cheapest and most isolated first, the snapshot last. Each
seam is one Claude Code session, one branch, gates green before and after.
No seam changes a byte on disk or on the wire until seam 7, and seam 7 is
grandfathered (see Encoding policy).

1. **Core as a CMake boundary.** Make `contract/`, `cluster/`, `storage/`,
   `ledger/`, `subsystem/` and the root utilities a library target
   (`dannan`) that `fuse/`, `media/`, `catalogue/`, `filesystem/`,
   `torrent/`, `acquisition/`, `playback/`, `api/` link against, never the
   reverse. Gate: the build fails on any include from core into a hosted
   folder.
2. **Open the RPC type space.** Replace the closed `MessageType` enum with
   a core-reserved range and a registration call on `MessageRoutes` that a
   subsystem uses to bind its own types, carrying lane and work class.
   Existing values keep their numbers. Gate: protocol version unchanged; a
   torrent plugin binding through the new call passes the acquisition
   tests.
3. **HTTP route registration.** `HttpServer` gains
   `register_route(prefix, class, handler)`; the prefix list in
   `service.cpp` becomes the FS's registrations. Gate: every endpoint in
   `docs/api.md` answers identically, including lane classification.
4. **Generic capability registry.** `SubsystemRegistry` becomes
   typed-by-name publication; `publish_torrent` and `publish_fuse` become
   two names. Gate: FUSE and torrent plugins load and run unchanged.
5. **Claimed collection lifted.** `torrent_request`'s field-wise LWW merge,
   epoch claim and tombstone become a core template; `TorrentRequest` is
   its first instantiation with identical bytes. Gate: the three-way merge
   tests in `torrent/` pass against the template; duplicate-after-partition
   still lands as `(2)`.
6. **Horizon builder decoupled.** `NodeHorizonBuilder` takes a
   `ReferencedSets` provider interface; the catalogue supplies one. Gate:
   GC gates unchanged; claims survive a concurrent branch as before.
7. **Head state becomes opaque.** `MetadataSnapshot` splits into the core's
   head record (parents, certificate, origin dots, clock) and an
   owner-supplied state blob with an owner-supplied codec and merge. The FS
   owner's codec is defined as the current SM15-18 and DLT9/10 encodings,
   so the bytes on disk and on the wire do not change. `metadata_merge.cpp`
   moves to `filesystem/` as the FS merge. Gate: a 0.91 node and a seam-7
   node interoperate in the chaos cluster for one full week; layout `v18`
   and protocol 23 unchanged.
8. **SubsystemContext becomes the kernel API.** The context hands a hosted
   system: object store, claims, leases (seam 5), heads (seam 7), channels
   on the lanes (seam 2), tickets, budgets by class, fault sink, time, Lugh
   principal and scope. It no longer hands `filesystem` or `hydration`; the
   FS obtains those from its own registration. Gate: the FS runs as a
   hosted system through the context alone.

After seam 8, Macha's FS is a hosted system and the door exists. The second
hosted system is built through that door and nothing else.

## Encoding and protocol policy

The extraction changes no byte on disk or on the wire. The one place that
is hard, the snapshot encoding, is handled by grandfathering: the FS
owner's codec for its opaque state blob is defined as the existing
encoding, so the generic head is byte-compatible with 0.91 by
construction.

The cost is that the core's first opaque-state format is shaped by the
FS. That is accepted. A layout bump (`v18` to `v19`) and a protocol bump
(23 to 24), with a `macha-namespace-migrate`-style one-shot, are reserved
for the first time a second hosted system needs something the FS encoding
cannot express. Macha's fresh-genesis contract already makes that an
ordinary operation, and a rolling upgrade is whole-cluster regardless.

Rule for every seam: if a diff touches a codec, the seam is wrong.

## Repo split and the second hosted system

The core stays in-tree until two hosted systems exist. A split before that
produces a library whose boundary nobody has pulled on from two sides; the
diff between what the FS needed and what the second system needed is the
design input for the split.

The second hosted system should be the one that stresses the door
differently from the FS. The event system does: it needs channels with
explicit delivery contracts, heads for retained topics only, no extents,
and a client protocol gateway. If it can be built through
`SubsystemContext` with no core edit, the door is right. If it needs a
core edit, that edit is the next seam, applied to the core and proven on
the FS before the event system uses it.

Only then: Dannan becomes its own repository, Macha pins a tag, and the
chaos cluster runs both.

## Open decisions

- [ ] Does Head stay in the core, or become Object + Claim + a convention?
      This plan keeps it in: the core knows what divergence is and calls
      the owner's merge. Decide before seam 7.
- [ ] Lugh scope check at the write floor: on the credential
      (partition-safe, slower revocation) or looked up at write time
      (immediate revocation, needs a Lugh replica reachable). Decide before
      seam 8.
- [ ] Name: decided, Dannan. The distributed substrate, the virtual PHY;
      every project runs on an implementation of it.
- [ ] Which providers for the chaos cluster, and the monthly budget it may
      consume.
- [ ] Whether seam 2's registration also covers the
      `is_notification_message()` gate on `request_id == 0`, or
      notifications stay a core-only kind.
