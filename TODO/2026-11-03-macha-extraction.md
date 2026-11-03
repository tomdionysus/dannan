# Macha extraction

**How to read this since 2026-11-03, evening.** Tom reversed "in-tree
first": Dannan and Dagda are written in `~/devroot/dannan` from the
design, and Macha is ported onto them (`ACTIVE.md`, Decisions and Next).
This document's direction is therefore inverted, and its parts are used
as follows. The file table ("Where each file goes") still says which
Macha file each Dannan or Dagda piece must replace and what stays Macha's.
"The seam gate" is the port gate, run before and after every port step.
Each seam's "What must not change" and "Gate" still hold for the port
step that replaces the same piece (seam 0 is step 3, seams 1 and 2 are
step 6, seam 3 is dissolved into the pins, seam 4's reserved numbers are
Dannan's route table from its first commit, seams 5 to 9 are steps 6 and
8), and the "What moves" and "What changes" paragraphs describe the
behaviour and the Macha-side shape, not a move. "After the seams"
(level 0, identity, AthenaSIP) stands as written. Nothing below this
paragraph has been edited for the reversal.

Status: proposal, 2026-11-03. The seam sequence of Tom's "Macha Core
Extraction Plan" (2026-11-02) worked against the tree as it stands, under
the "Dannan: Distributed Substrate Library Plan" of the same day, which
makes it two libraries. Nothing here is built. Each seam ships alone: one
session, one `feature/` branch off Macha's `develop`, the gate green
before and after, merged before the next starts. File and line references
are to `../macha` at `6c7ec64` (0.91.2, the baseline); re-check before
each seam. Tom's answers of 2026-11-03 (ACTIVE, Decisions) are folded in:
the Asio rewrite is seam 0, the store is Dagda, head spaces carry a
replica policy from the start, and there are two protocol bumps.

## The rule this proposal rests on

**Macha loses no function.** Every behaviour of the code that moves is
still Macha's behaviour afterwards, and the proof is named before the
move. The inventory
([`2026-11-03-macha-function-inventory.md`](2026-11-03-macha-function-inventory.md))
is the list; this plan says in what order the code moves and what each
move may and may not touch.

Two mechanical rules follow. A move is a move: a commit that moves a file
does not change its content, and `git diff -M` reviews it as a rename. No
byte moves: nothing on disk or on the wire changes until step 4 (identity),
and a seam whose diff touches a codec is wrong. The grandfathering rule
(Encoding policy, below) is how the heads become opaque without a byte
changing.

## What exists now (survey, 2026-11-03)

`macha_core` is one shared library of 108 translation units
(`CMakeLists.txt:275`), dlopen'd by the FUSE and torrent plugins and linked
by the executable and five tools, with one build identity across all of
them. Every subsystem lives in a folder under `src/`; includes are relative
to `src/`. The parts the designs call Dannan and the store are mostly in
`contract/`, `cluster/`, `subsystem/`, `storage/`, `ledger/` and
`metadata/`, and the six entanglements the extraction plan names are
exactly where the filesystem reaches into them. Two more entanglements
the plan does not name sit above those six, and they come first:
`config.hpp` is one struct for the whole node and is included by
`subsystem.hpp`, `cluster.hpp` and `local_state.hpp`; and `NodeRuntime`
(`cluster/cluster.hpp:75`) includes the stores, the metadata snapshot, the
sessions and the users, so the control plane cannot be a library while it
exists in that shape.

### Where each file goes

Three libraries. "dannan" is the network layer; "store" is the separate
library whose name is open; "macha" is the filesystem, the hosted system.
A file marked *split* is divided by the seam named; until then it stays
where it is.

| Today | Goes to | Note |
|---|---|---|
| `types.{hpp,cpp}` | dannan | `SessionIdentity` leaves for `auth/` (macha) at seam 2; the rest is Dannan's vocabulary. `Hash256` stays here: a 32-byte hash is not a store noun. |
| `codec.{hpp,cpp}`, `crypto.{hpp,cpp}` | dannan | Wire encoding and the cluster keys; the store uses both. |
| `config.hpp`, `config_base.cpp` | *split*, seam 1 | Three structs, one YAML schema. |
| `log`, `diagnostics`, `supervised`, `process_allocator`, `retained_memory.hpp`, `retry_policy.hpp`, `resident_bytes.hpp`, `startup_progress.hpp`, `observation`, `durable_file`, `json`, `coverage`, `version.hpp.in` | dannan | Root utilities every library uses. `durable_file` is how the roster is written; `observation` is the window mechanism, its gauges are each hosted system's. |
| `write_behind.{hpp,cpp}` | store | The journals' write-behind; confirm at seam 3. |
| `contract/work`, `thread_safety.hpp`, `time_source.hpp`, `walk.hpp`, `published.hpp` | dannan | Work classes, waits and the guard; budgets, cursors and pages; published snapshots. |
| `contract/object_store.hpp`, `claim_store.hpp`, `object_ledger.hpp`, `id_lookup.hpp`, `predicates.hpp`, `horizon*.hpp`, `gates.{hpp,cpp}` | store | The object ledger spec's contracts and the three destructive gates. |
| `contract/metadata_view.hpp` | macha, over the store | FS-typed (`FsEntry` paging); becomes Macha's facade over the store's heads at seam 8. |
| `cluster/net.*`, `message_routes.hpp`, `frame_type.hpp`, `rpc_refusal.hpp`, `membership.*`, `placement.*`, `node_identity.*`, `public_connectivity.*`, `miniupnpc_compat.hpp`, `activity_clocks.*`, `node_events.hpp`, `telemetry.*` | dannan | Lanes, channel, routes, roster, placement, identity, UPnP and self-probe, activity, events, the telemetry transport. `telemetry.hpp`'s record keeps its storage, cache and playback fields, grandfathered. |
| `cluster/cluster_node.hpp` | dannan | Minus `advertise_storage`, which becomes the store's advertisement call (seam 2). |
| `cluster/cluster.{hpp,cpp}` (`NodeRuntime`) | *split*, seam 2 | Control plane to dannan; advertisements and generation announcements stay with their owners. |
| `cluster/node_resources.*` | *split*, seam 2 | `StorageLock`, `ActivityClocks`, `RetainedMemoryLedger`, `NodeEvents` to dannan; `DataResourceArbiter` to store; `TranscodeRateBook` stays macha. |
| `cluster/data_work.hpp` | store | `DataWorkContext`: DATA-plane provenance and credit. |
| `cluster/distributed_store.*`, `replica_selector.*`, `storage_server.*`, `control_objects.*`, `local_state.*`, `local_services.*` | store | The distributed object store, its serving side, control-object fetch, local-state recovery and the servers built from it. |
| `cluster/transcode_rates.*` | macha | Playback facts. |
| `subsystem/*` | dannan | Supervisor, registry, ABI; the context is split at seam 9. |
| `storage/*` | store | Local stores, pool, packs, durability domains, I/O pressure, cache, retention, journals. |
| `ledger/object_trie`, `trie_diff`, `trie_union`, `held_ledger`, `reference_counts`, `retention_ledger`, `predicate_query` | store | The trie and everything on it. |
| `ledger/node_horizon_builder.*` | store, after seam 7 | Includes `catalogue/` today. |
| `ledger/availability.*` | macha | The survey's result type and `keep_unavailable`. |
| `metadata/metadata.{hpp,cpp}` | *split*, seam 8 | Head record to store; `FsEntry`, conflicts and the FS state to macha. |
| `metadata/metadata_manager.*`, `metadata_server.*`, `namespace_control_store.*` | store | The commit DAG, acceptance, head sets, the replica's serving side, control objects. |
| `metadata/namespace_tree.*` | store | The content-addressed tree over keys; its leaf codec is the FS's, grandfathered. Re-check at seam 8. |
| `metadata/metadata_merge.cpp` | macha (`filesystem/`) | The FS merge, called by the store on divergence (seam 8). |
| `torrent/torrent_request.*` | *split*, seam 6 | The template to `store/lease/`; `TorrentRequest` stays macha with identical bytes. |
| `service/claim_walk.*` | store | A trie walk, no peer. |
| `service/maintenance*`, `availability_service.*`, `node_services.*`, `service.*`, `service_config.cpp`, `main.cpp` | macha | The FS's maintenance pass calls the store's sweeps through their ports; split further only if seam 9 needs it. |
| `auth/`, `http/`, `api/`, `filesystem/`, `fuse/`, `media/`, `playback/`, `catalogue/`, `acquisition/`, the rest of `torrent/` | macha | The hosted system. HTTP is Macha's by the Dannan plan. |
| `tools/*` | macha | They decode FS state; they link all three. |
| `tests/*` | by owning invariant | A case moves with the code it proves; the suite names stay until the split. |

### The seam gate

Run before and after every seam, on the laptop, then on fi-1:

1. The three suites (`./run-tests.sh build`): counts equal to or above the
   baseline recorded in `COMPLETED.md`; no case deleted; a moved case
   counted in its new file.
2. The config golden: `macha.yaml.example` and every fixture under
   `tests/fixtures/` parsed and normalized, dumped field by field, equal to
   the committed dump; every unknown-key refusal and every
   `configuration_warnings` line unchanged.
3. The Status golden: `GET /api/v1/status`, `/status/nodes` and
   `/status/diagnostics` from a two-node in-process `TestCluster`, with
   the volatile fields masked, equal to the committed fixture.
4. The route list: every route in `docs/api.md` still answered with the
   same lane class (`test_web_api.cpp` walks it).
5. Plugins: the seven test plugins load, decline, fault and are refused
   exactly as before (`test_subsystem_supervisor.cpp`); the real FUSE and
   torrent plugins load from the build tree.
6. The install: `cmake --install` into a scratch prefix produces the same
   file list plus, from seam 3, the two new libraries under
   `lib/macha/`; every executable and plugin resolves them by rpath from
   that prefix; `build/deploy.sh`'s staging reads the same tree.
7. Wire and disk: protocol 23 in the handshake; layout v18 in the state
   and backend markers; from seam 8, the two-process mixed-build test
   (baseline binary against seam binary over loopback) forms a cluster,
   commits both ways and merges a divergence.
8. The diff: `git diff -M --stat` shows moves as renames; every content
   change is named in its commit.
9. The inventory: each row the seam touched carries its destination and
   the case that proves it there.

## The seams

### Seam 0: the transport on Asio

Tom, 2026-11-03: Boost.Asio from the start, first, before everything, on
0.91.2 as it stands.

**What moves.** Nothing. `cluster/net.cpp` (4,674 lines) is rewritten in
place.

**What changes.** The transport's mechanism and nothing it does. Today
each dialled peer connection runs two persistent threads (reader and
writer) with multi-megabyte DATA buffers, the client runs a health thread,
the server runs an accept thread, a session thread per connection and
worker pools per request class, and a call is a `std::future` with
cancel, abort, promote and idle callbacks (`AsyncRpc`). On Asio: one
io context for the sockets with a strand per connection and lane, the
reads and writes as asynchronous operations, timers from the io context,
and the worker pools kept as they are, because handlers are compute and
the laws want them bounded per class. `AsyncRpc` keeps its surface and
gains a completion form that posts to a caller-supplied executor
(`any_io_executor`), which is the executor rule made native; the blocking
`call` is a wait on that completion with its declared waits unchanged.
`RpcLinks` stays the seam between the client and its sockets so
`NetworkLinks` and the tests' fakes keep their shape. Boost (Asio, at
least 1.87 as AthenaSIP requires) becomes a dependency of `macha_core`
and of the dependency-light test build; `CONTRIBUTING.md`'s line that
Boost is not otherwise required is updated.

**What must not change.** Every row of the inventory's "Transport and
lanes" table: the two lanes and the probe lane, frame priority order,
BBR with keepalive and no Nagle on every socket, the outbound queue
limits and the small-message reserve, reassembly limits and the memory
leases that follow a message, the pending-reply limit, stall notices and
no-progress deadlines per type, cancel, abort and promotion on both
sides, the frame size negotiation, `SecureChannel`'s AEAD and counters,
the handshake and protocol 23, the request classes and the fast-control
allow-list, executor cancellation on disconnect and shutdown, the three
broadcasts, reverse dial and maintained peers for inbound-incapable
nodes, the dial-back probe and its rate limit, session reaping, peer
health and latency, traffic totals. Lock order and the thread-safety
annotations stay; a mutex that an Asio strand makes redundant is removed
in a later commit, not this one.

**Gate.** The seam gate; `test_rpc_cluster.cpp` whole, every case; the
two-process test against the 0.91.2 binary (the wire is identical, so a
0.91.2 node and a seam-0 node form a cluster, exchange membership, move
objects on both lanes and commit metadata both ways); the Status golden
(RPC stats and work stats are fields of it); a deploy to fi-1 alongside
the laptop, since the thread model is what the single node exercises
hardest.

**Risk.** The largest single change in the plan, made before any
boundary exists to contain it. Its protection is the inventory and the
`rpc_cluster` suite, which is why item 1 under Next (the proof column for
the transport rows) is done before this seam starts. The laptop must
have one Boost (Waiting on Tom, 2).

### Seam 1: the config split

**What moves.** Nothing yet; this seam makes the move possible.

**What changes.** `Config` (`config.hpp:547`) becomes three structs, one
per library, and `Config` itself a struct of the three, so every current
reader keeps compiling (`config.runtime.memory_bytes` becomes
`config.dannan.runtime.memory_bytes` by a one-line change at each site,
or an accessor keeps the old spelling for the seam). The YAML keys, the
parser (`config.cpp` in the executable), `normalize_config`,
`configuration_warnings` and the refusal of unknown keys do not change.

| To `dannan::Config` | To `dagda::Config` | Stays Macha's |
|---|---|---|
| `state_path` (the node's own files: id, roster, inbound resolution, lock), `key_file`, `plugin_path`, `listen_host`, `advertise_host`, `failure_domain`, `node_name`, `port`, `inbound_capable` and its reprobe intervals, `dial_back_probe_min_interval`, `upnp`, `external_ip`, `connectivity_check`, `bootstrap`, `heartbeat`, `telemetry_interval`, `dead_after`, `connect_timeout`, `max_frame_size`, `control_stall_notice`, `data_stall_notice`, `control_no_progress_deadline`, `data_no_progress_deadline`, `runtime.*` (memory limit, the three reserves, reassembly reserve, glibc arenas), `service_startup_no_progress`, `service_startup_timeout`, `log_level` | `storage_backends`, `hosts_extents`, `storage_packing`, `ledger_cache`, `metadata_store`, `cache`, `maintenance`, `replication`, `metadata_write_copies`, `write_copies`, `retention_check_batch_size`, `retention_check_concurrency`, `write_stall`, `data_inflight_bytes`, `data_viewer_reserve_bytes`, `io_pressure_*`, `data_credit_no_progress_deadline`, `metadata_cache`, `metadata_materialization_cache_bytes` | `filesystem`, `fuse`, `catalogue`, `ingest`, `torrent`, `streaming`, `hydration`, `session`, `web`, `extent_size`, `read_ahead_extents`, `ffmpeg_log_level`, `config_file` |

`ClusterNode::config()` returns the whole struct for now; the store parts
that read it switch to their own struct at seam 3.

**What must not change.** One key, one name, one default, one refusal.
`macha.yaml.example` and `docs/configuration.md` are untouched.

**Gate.** The seam gate, item 2 above all; `macha-tests-runtime`'s config
cases; the `tests/fixtures/` configs.

**Risk.** Keys the designs place in one library that two libraries read
(`maintenance.garbage_grace_ms` sets the roster's forgetting horizon in
Dannan and the GC grace in the store). Rule: the key belongs to the
library that defines its meaning; the other reads it through that struct.

### Seam 2: the `NodeRuntime` split

**What moves.** Nothing between folders; the class divides in place.

**What changes.** `NodeRuntime` becomes two classes. The first, Dannan's
node (keep the name `NodeRuntime` for the seam; it is renamed at the
split), owns what `cluster.hpp:75` builds before storage and metadata so
the node is reachable while state recovers: the identity, `Membership`,
`PublicConnectivity`, `TelemetryStore`, `RpcClient`, `RpcServer`, the
control routes it binds (`ping`, `members`, `telemetry`,
`identity_resets`, `dial_request`, `dial_back_probe`), the membership
loop, the connectivity loop with the `inbound_capable: auto` resolution
state machine and its persisted file, the telemetry loop, `call`,
`call_async`, the three broadcasts, peer latencies, the per-message stall
notice and no-progress deadline tables, `apply_identity_reset` and
`propagate_identity_reset`, `refuse_impossible_cluster`, readiness of the
control plane, traffic totals, `peer_viewers_active`, `request_stop`,
`cancel_outbound_calls` and `stop`.

The second is Macha's facade over it, holding what the stores and
metadata advertise through the node today: `advertise_storage`,
`advertise_storage_backends`, `advertise_metadata_generation`,
`advertise_cache`, `publish_self`, `announce_metadata_generation`,
`remote_metadata_generation` and its epoch and settle loop,
`metadata_announcements`, the `hosts_extents` resolution
(`resolve_hosts_extents_for`: automatic is "has DATA backends and is
inbound-capable"), `wait_local_state_ready`, `readiness()` beyond the
control plane, `reconfigure_local`, and `enforced_playback_budgets`. At
seam 3 the metadata announcements go with the store and the playback
budgets stay with Macha; the facade exists so that this seam is a divide,
not a redesign.

`ClusterNode` loses `advertise_storage`; the store's `StorageServer`
advertises through the facade. `RecoveryProgress` stays in Dannan as the
mechanism, with its planes declared by name by whoever recovers them (the
store names `data_storage`, `control_storage`, `cache`, `retention`,
`metadata`); the Status vocabulary (`startup.phase`) is unchanged.

The gossiped `NodeInfo` (`types.hpp:52`) and `NodeTelemetry`
(`telemetry.hpp:60`) records keep every field and every byte. Their
storage, cache, metadata-generation and write-floor fields are filled by
the facade through the node's advertisement calls, exactly as today, and
read by placement and the write-floor check through the same accessors.
A generic advertisement map replaces them at the protocol bump (step 4).

**What must not change.** Lock order and mutex ownership: each mutex moves
whole with the state it guards and the comments that state the order
(`inbound_persist_mutex_` before `inbound_mutex_`; the client's `mutex_`
before membership's). Every `MessageType` is still answered. Status is
byte-identical (gate item 3). The dial-back probe's per-asker rate limit,
the `inbound_capable` persistence and the `refuse_impossible_cluster`
check (no inbound-capable node anywhere) keep their tests.

**Gate.** The seam gate; `test_rpc_cluster.cpp` whole, with
`test_inbound_incapable_node_is_reached_only_over_its_own_sessions`,
`test_rpc_slow_control_does_not_abort_data`,
`test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` and
`test_two_node_mutual_bootstrap_metadata_write_floor` named in the
handoff; `test_node_services.cpp`, `test_service_root.cpp`.

**Risk.** `NodeRuntime` is the root's hub and every service takes it by
reference. Keep the constructor signature for the facade so
`Service` (`service/service.cpp:28`) and `NodeServices` change only
their member types. A session that finds itself redesigning the root has
left the seam.

### Seam 3: the CMake boundary

**What moves.** Every file in the table above whose destination is dannan
or store and is not marked *split*, into `dannan/` and `dagda/` beside
`src/`, each with `include/` for the public headers and `src/` for the
rest, in the layout the Dannan plan gives (`reach/`, `net/`,
`membership/`, `placement/`, `sched/`, `supervise/`, `identity/`,
`status/`, `contract/`, `test/`; and `domain/`, `store/`, `ledger/`,
`heads/`, `lease/`, `contract/` for the store). The include paths change
with the move and nothing else does: `#include "cluster/net.hpp"`
becomes `#include "dannan/net/net.hpp"` by the one mechanical commit the
move is.

**What changes.** Three shared libraries, `libdannan`, `libdagda` and
`libmacha_core`, each private under `lib/macha/`, linked in that order;
the plugins and executables keep one rpath hop; `kBuildIdentity` is
stamped once and checked across all three (the plugin ABI test
`test_plugin_mismatched_abi` proves the refusal still works). An
include-direction case in `tests/test_invariants.cpp` reads the tree and
fails on any include from `dannan/` into the store or Macha, or from the
store into Macha. `macha_thread_safety()` is applied to all three.

**What must not change.** The install layout beyond the two new files;
`MACHA_BUILD_SERVER=OFF` still builds the dependency-light suite;
coverage and sanitizer builds still instrument whole-tree (the sanitizer
comment at `CMakeLists.txt:107` applies to three libraries now).

**Gate.** The seam gate, items 5 and 6 above all; a clean configure on
the laptop (macOS, Homebrew FFmpeg and libtorrent prefixes) and on fi-1.

**Risk.** Three shared libraries mean three copies of nothing: every
global (the process allocator, the wait guard's mode, the steady time
source, log state) must live in exactly one of them. Audit the root
utilities for statics before the move; the coverage runtime's per-image
copy (`src/coverage.hpp`) needs the same treatment per library.

### Seam 4: the open route space

**What changes.** `MessageType` (`cluster/net.hpp:28`) stops being a closed
enum: it becomes a 16-bit value with named constants in each library,
every existing number kept. `MessageRoutes` (`cluster/message_routes.hpp`)
widens its table (`route_count` is 128 and 127 is in use) and gains
`register_type(number, name, lane, class, notification, handler)`;
`bind` becomes the registration's handler half. A registration is refused
when the number is taken or reserved, or when it names a lane class it
may not (a hosted system cannot register a control-lane type that is not
a notification). `RpcClient::lane_for`, `default_frame_type`,
`message_type_name`, `is_notification_message` and the server's
`fast_control_request` allow-list read the registration instead of a
switch. The extraction plan's open question (whether notifications are
registrable) is answered yes: `notification` is a field of the
registration, and the `request_id == 0` rule stays Dannan's.

Reserved to Dannan: `ping` 1, `members` 2, `telemetry` 24,
`identity_resets` 25, `dial_request` 43, `dial_back_probe` 44,
`promote_read_ahead` 14, `promote_foreground` 15, `cancel_transfer` 16
(transfer control is the lane's), `ok` 100, `error` 101, `members_reply`
102, `bool_reply` 103, `telemetry_reply` 109, `identity_resets_reply`
110, `dial_back_probe_reply` 120, and the retired 37, 38, 48 and 124,
which are never reused. Registered by the store: 3 to 11, 13, 17 to 23,
26 to 32, 40, 41, 45, 47, 49 and their replies 104 to 108, 111, 112,
118, 121, 123, 125. Registered by Macha: 12 (`session_retire`), 33 to
36, 39, 42, 46, 50, 51 and 113 to 117, 119, 122, 126, 127.

**What must not change.** Numbers, lanes, frame types, the notification
rule, the fast-control allow-list's membership, protocol 23. The
acquisition tests pass with the torrent plugin binding `get_torrent_jobs`,
`torrent_job_action` and `torrent_intent` through the registration.

**Gate.** The seam gate; a two-node test registering a type at start and
round-tripping it; a refused collision; `test_rpc_cluster.cpp` whole.

### Seam 5: capabilities

**What changes.** `NodeInfo::flags` keeps its byte; `Membership` reads it
as a capability set: `inbound_capable` set is reachability class
`public`, clear is `dial-out` (level 0 adds `punchable` and `relay-only`
at the bump); `hosts_extents` set is the capability `store.data`.
`Membership::capabilities(node)`, `has(node, name)`; placement's
candidate filter asks for `store.data` where it asks `hosts_extents`
today; the two flag accessors remain as inline wrappers. Whether the six
spare bits of the byte can carry further capabilities before the bump
depends on the 0.91 decoder tolerating set bits it does not know;
check, and if so `relay` may ride early.

`SubsystemRegistry` (`subsystem/subsystem_registry.hpp`) publishes by
name: `publish(name, shared_ptr<T>)`, `withdraw(name, const T*)`,
`lookup<T>(name)`, with the same current-instance rule on withdraw;
`publish_torrent` and `publish_fuse` become `"macha.torrent"` and
`"macha.fuse"`. Macha also publishes `"macha.filesystem"`,
`"macha.hydration"` and `"macha.ingest"` for its own plugins, which is
what lets seam 9 take them out of the context.

**What must not change.** The roster record's bytes; `hosts_extents`
resolution; `inbound_capable` semantics; the FUSE and torrent plugins
load and run unchanged.

**Gate.** The seam gate; placement cases in `test_foundations.cpp`; the
inbound-incapable cases; `test_fuse_subsystem.cpp`; the acquisition
cases.

### Seam 6: the lease and claimed-collection template

**What moves.** The merge rules of `torrent/torrent_request.cpp` into
`dagda/lease/`: a `ClaimedCollection<Record, Traits>` with field-wise
last-write-wins by `(changed_ms, node_id)`, a dominating `cancelled`, the
max-wins tombstone with a retention window, the three-way merge over the
key set and a per-collection duplicate rule; and a `Lease` record
(`holder`, `epoch`, `since`) merged as `TorrentClaim` is today (higher
epoch, then earlier claim, then lower node id).

**What changes.** `TorrentRequest` is the first instantiation; its codec,
bounds (`max_torrent_requests`, `max_torrent_request_text`) and
`merge_torrent_requests` are expressed through the template with the same
bytes and the same answers. The template's API grows only what
`TorrentRequest` needs now; AthenaSIP's `acquire`, `renew`, `release`,
`holder` with an absence window are listed for step 5, not built here.

**Gate.** The three-way merge cases in `test_models.cpp` and the torrent
cases pass against the template; a duplicate after a partition still lands
as `duplicate_torrent` with the earlier `(created_unix_ms, id)` keeping
it; the SM15/SM16 encoding of `torrent_requests` round-trips byte for
byte against a fixture captured before the seam.

### Seam 7: the reachability provider

**What changes.** `NodeHorizonBuilder` (`ledger/node_horizon_builder.hpp`)
takes a `ReferencedSets` provider instead of `CatalogueManager&` and
`FileSystem&`: a hosted system supplies what its heads refer to (the
namespace's extents and tree nodes, the catalogue's manifest, shards and
artwork) and the builder computes the inventory and release horizons
from those sets, as `build_inventory` and `build_release` do today as
pure functions. The catalogue's `CatalogueRetentionSource` and
`CatalogueMaintenance` become one provider Macha registers; the ledger
stops including `catalogue/catalogue.hpp` and `filesystem/`.

**What must not change.** The three gates (`contract/gates.hpp`), the
grace period, the rule that a copy with any local claim is untouchable,
the partition safety of release (dots dominated by the observed clock
only).

**Gate.** `test_horizon_builder.cpp`, `test_ledger_gates.cpp`,
`test_claim_walk.cpp`, `test_catalogue_maintenance.cpp`; the GC cases in
`test_storage_metadata.cpp` and `test_absent_nodes.cpp`; a claim from an
unseen branch survives as before.

### Seam 8: opaque heads

The riskiest seam and the last that touches the FS's shape. Needs the
replica-policy decision and the two-process mixed-build test.

**What changes.** `MetadataSnapshot` (`metadata/metadata.hpp:142`) divides
in memory into the store's head record and the FS's state:

| Store head record | FS state (the owner's blob) |
|---|---|
| `generation`, `previous` and `merge_parents`, `hash`; `mutation_sequences` (the per-origin dots), `legacy_clock`; `metadata_write_replicas_required`; the acceptance certificate (`MetadataAcceptance`); the legacy retention fields `metadata_voters`, `metadata_participants`, `metadata_branch_floor`, `retention_baseline_complete` | `data_replication`, `extent_size`, `catalogue_root` and `catalogue_dot`, `namespace_root`, `entries`, `garbage`, `tombstone_batches`, `node_status`, `identity_resets`, `conflicts`, `torrent_requests` |

The store's `Heads` noun opens a named space (`macha/namespace`) with a
codec, a merge and a reachability provider (seam 7), and owns commits,
acceptance at the write floor, certificates, head sets, per-origin dots,
compaction, the Merkle tree over keys, and the reconciler: on divergence
it materialises both heads through the codec, calls the owner's merge
`(base?, a, b) -> state | conflict`, and commits the result with
`merge_parents`. `metadata_merge.cpp` moves to `filesystem/` as that
merge; `MetadataConflict` (it holds `FsEntry`) is FS state, so a conflict
record is part of the blob and the store's generic conflict record waits
for the first non-FS state. `MetadataView` becomes Macha's typed facade
over the space (`entries` paging, `common_ancestor_catalogue_root`, the
`mutate` and `mutate_delta` shapes), implemented on the store's
`read`, `mutate_local` and `commit`. Reads stay memory reads.

**Grandfathering, concretely.** The SM15 to SM18 and DLT9/DLT10 encoders
write head fields and FS fields interleaved in one record. The FS codec
is therefore a function of both halves: the store hands it the head
record and the blob, and it emits the legacy bytes; the decoder splits
them back. The blob is opaque to the store in the sense that matters (it
never reads a field of it) and not in the sense of a separate byte
range, which arrives with layout v19. `macha-metadata-dump` reads the
same files the same way.

**Replica policy.** `open()` takes `replicas: N` or every-node from the
start (Tom, 2026-11-03). The holders of a space are
`rendezvous_nodes(space name, roster, N)`, the plain HRW placement
already in `cluster/placement.hpp`, re-evaluated as the roster changes,
with the write floor per space (`metadata_write_copies` is Macha's value
for its space). A node that does not hold a space forwards a read over
CONTROL to a holder and says so in the evidence; a node that holds it
answers from memory. Macha's `macha/namespace` is opened every-node, so
nothing Macha does changes; the `replicas: N` path is proven by a space
opened at three replicas on a five-node in-process cluster, read from a
non-holder, written while a holder is partitioned, and repaired when it
returns.

**What must not change.** Layout v18, protocol 23, every byte of every
record; the write floor and the acceptance rule; "accepted heads are a
set"; reconciliation with no common ancestor; conflicts as durable
records; idempotent retries by dot; history truncation thresholds;
`macha-namespace-migrate`'s re-rooting; the tombstone batches.

**Gate.** The seam gate with item 7 (the mixed-build test: a baseline node
and a seam node commit both ways, diverge under a partition the test
makes and merge, the result identical from either side);
`test_metadata_merge.cpp`, `test_namespace_tree.cpp`,
`test_namespace_migration.cpp`, `test_storage_metadata.cpp`,
`test_absent_nodes.cpp`, `test_namespace_entries.cpp`; the
`rpc_cluster` metadata cases
(`test_metadata_history_checkpoint_concurrent_proposers_converge` named);
`macha-metadata-dump` output identical on a fixture state directory;
then, on the cluster, a 0.91 node and a seam-8 node together for the
period the extraction plan names.

### Seam 9: the context as the API

**What changes.** `SubsystemContext` (`subsystem/subsystem.hpp:41`)
becomes `dannan::Context` with the nouns the Dannan plan lists (Nodes,
Routes, Channels, Identity, Budget, Faults, Time, and Status as one
surface per node that every hosted system publishes into: Tom,
2026-11-03) and
`dagda::Context` over it (Domains, Objects, Claims, Heads, Leases,
Tickets, Retained), each a spec a second implementation could satisfy.
`filesystem`, `hydration`, `ingest` and `local_state` leave the context;
the FUSE and torrent plugins look them up by name in the registry (seam
5). Macha's root (`service/node_services.hpp`) builds the FS through the
two contexts and nothing else; the gate is that it compiles against the
contexts' headers alone.

Channels in v1 are the existing route mechanism with a declared contract:
`notify` is `broadcast_notify` on CONTROL; `bulk` is a DATA-lane call;
`stream` and `retained` are specified and built when the event system or
AthenaSIP needs them (step 5 needs `retained`). The executor rule is
native since seam 0: every context call completes by posting to the
caller's executor, and the blocking forms Macha calls (`call`, `put`,
`commit`) are waits on those completions with their declared waits.

**What must not change.** Every plugin behaviour the supervisor tests
prove; the FS's start order and stop order (`node_services.hpp`'s graph);
the fault sink and restart policy; the `startup_stop` token for FUSE's
blocking construction.

**Gate.** The seam gate; `test_subsystem_supervisor.cpp`,
`test_fuse_subsystem.cpp`, `test_node_services.cpp`,
`test_service_root.cpp`, `test_filesystem_fuse.cpp`; the FS runs as a
hosted system on the two-node cluster with the full gate.

After seam 9 the door exists, and the second hosted system (AthenaSIP)
comes through it and nothing else.

## After the seams

### Level 0 (step 3)

New code in `dannan/reach/`: observed candidate addresses learned from
every inbound frame and confirmed only when another node has reached
them; path classes (`direct`, `punched`, `relayed`) tried in order per
pair; the punch broker (a public node both already talk to tells each the
other's observed address and a time, both connect, the TCP simultaneous
open that completes wins; Tom, 2026-11-03: TCP first, a UDP lane later if
real clusters show it is worth it); relay by a node advertising `relay`,
carrying ciphertext only,
CONTROL relay unconditional and DATA relay charged to the relaying node's
loader class and published in Status; a path change as a transport
reconnect under the same node identity, with `ClusterNode` unchanged and
in-flight calls retried under the existing policy. Macha's
`inbound_capable: false` dial-out path is level 0's `direct` half-case
and stays as it is. Protocol 24 (the first bump): path classes and
candidate addresses in the roster record, the punch broker's messages,
the relay framing. Proof: half the compose cluster relay-only, the same
gates; then the chaos cluster when there is one.

### Identity (step 4)

`athenasip/src/cluster_ca.cpp` ported to `dannan/identity/` in Macha's
in-tree style (the EC P-256 CA, pathlen 0, two-year node certs with SANs
naming the node id, `issue_node` with `replace`); Macha's single
`ClusterKeys` becomes a CA with one node class; `SecureChannel` keys from
the certificate; mutual authentication at the handshake, the dialled
address checked against the SANs. Protocol 25 (the second bump), rolling,
and the point at which `NodeInfo` and the telemetry record gain generic
advertisement maps and the FS fields leave them (the telemetry record
becomes the gossip of Dannan's Status surface, which Macha's fields are
published into like any hosted system's), and at which the capability set
leaves the flags byte. Scope on the write floor is looked
up in the Lugh head by every replica that counts toward the floor; a node
without the Lugh space accepts a short-lived credential's scope (Tom,
2026-11-03).

### What AthenaSIP needs (step 5)

The port leaves the SIP stack untouched and replaces two drivers. Each
row is a Dannan or store noun the extraction must have left in a shape
AthenaSIP can use from one non-blocking strand, with completions posted
to that strand (the executor rule) and reads of a held head as memory
reads.

| AthenaSIP today | Where | Becomes |
|---|---|---|
| `Datastore` records: realms, subscribers, bindings, trunks, users, sessions, call records, presence, reoffers, trunk status | `src/datastores/datastore.h`, Redis at `redis_datastore.cpp` | Heads in spaces `athena/realms`, `athena/subscribers`, `athena/bindings`, `athena/trunks`, `athena/users`, `athena/calls`, `athena/presence`, each with a codec and a merge (last-writer-wins by the record's own `updated_at`; bindings and presence with the store's expiry as the merge's tombstone) |
| `Datastore::lease` | `datastore.h:186`, `redis_datastore.cpp:447`, used by `trunk_registrar.cpp:175` (two-minute takeover), presence takeover, and the orphan-call sweep (`core.cpp:1642`, the lowest live node id today) | `Leases::acquire(name, scope, absence)` from seam 6's template; every action under a lease carries its epoch; the sweep becomes a lease on `athena/sweep` |
| `Datastore::counter_add` and `counter_get` | `datastore.h:196`, `redis_datastore.cpp:466` | A per-origin counter merged by sum, with a window |
| `EventSystem::publish`, `subscribe`, wildcards | `src/events/event_system.h`, MQTT at `mqtt_event_system.cpp` | `notify` channels on CONTROL with topic filters; at-most-once, lossy by design (`presence/<aor>/changed`) |
| `publish_state` and `will_set` (retained `nodes/<id>/status`) | `event_system.h:149`, `:153`; read by `NodeDirectory` (`src/node_directory.h`) | The `retained` contract: a notify channel whose last value lands in a head; node status from Dannan's gossip and membership, so `NodeDirectory` and its three-interval staleness retire |
| `AddressDiscovery` (STUN, `received` observation, ten-minute peer probes, `cluster_unreachable`) | `src/address_discovery.h` | Level 0: observed addresses, confirmation by reach, path classes; the node is told the path class per peer if it asks |
| Cluster CA and mutual TLS on the inter-node listener | `src/cluster_ca.cpp`, `docs/clustering.md` | Dannan's identity (step 4); the SIP inter-node listener keeps using the same certificates |
| The plugin contract (`Executor`, `Handler<T>`, `Status`, `_complete`, `API_VERSION` 24) | `src/plugins/plugin.h` | Unchanged: the two Dannan-backed drivers implement it; the registry refuses a mismatch as today |

Gate: the unit suite with Redis and MQTT set still passes with the old
drivers (they stay until no deployment names `redis://` or `mqtt://`),
and `test/e2e/cluster.sh` passes on the Dannan drivers with the second
node behind CGNAT and no public address.

### The split (step 6)

Dannan and Dagda move to this repository; `namespace macha` becomes
`dannan::<subsystem>` and `dagda::<subsystem>` in one mechanical commit
with the thread-safety macro prefixes (`MACHA_GUARDED_BY` and the rest),
the reformat to AthenaSIP's mechanics (`.h`, the header block, `_name`,
Google base at 160 columns), the file renames and the port of the moved
tests to GoogleTest; Macha and AthenaSIP pin a tag; `kBuildIdentity`
becomes Dannan's version plus the pinned tag.

## Encoding and protocol policy

Restating the design's rule with the records it binds:

- The metadata snapshot (SM15 to SM18, DLT9/DLT10): grandfathered as the
  FS codec at seam 8.
- The roster record `NodeInfo` and the `members` exchange: unchanged
  through seam 9; path classes and candidates added at protocol 24; the
  flags byte carries the two capabilities until protocol 25, when generic
  advertisements arrive.
- The telemetry record: unchanged; Macha's fields in place until protocol
  25.
- The wire frame, fragments, `MessageType` numbers, lanes and priorities:
  unchanged through the Asio rewrite (seam 0) and the registration (seam
  4), which describe what is already on the wire; the punch broker and
  relay messages arrive at protocol 24.
- The handshake: protocol 23 until step 3, 24 until step 4; keys from
  `ClusterKeys` until protocol 25, then certificates.
- State layout v18 and backend markers: unchanged until the first state
  the FS encoding cannot carry.

## Open questions

Beyond those under Waiting on Tom in `ACTIVE.md`:

- Whether `write_behind` is the journals' alone (store) or also the
  roster's (Dannan): read its users at seam 3.
- Whether the 0.91 roster decoder tolerates unknown bits in
  `NodeInfo::flags` (seam 5); if not, `relay` waits for the bump.
- Whether `namespace_tree` is generic enough to be the store's at seam 8
  or is FS code with a store-shaped hole; decide from its leaf codec.
- Which of `RpcServer`'s execution limits (`metadata_workers`, the
  per-class pending bytes) are Dannan's per-class budgets and which the
  store's: `metadata_*` names a store queue on Dannan's server. Decide at
  seam 4, where the registration carries class.
