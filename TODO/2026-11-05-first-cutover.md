# First cutover: Macha on Dannan's transport and node

**Baseline moved, 2026-11-05: Macha `develop` at `cac5998`, tag 0.92.4 on `70cf4a4`.** Citations below were read at `00861dd`. In the moving files only `src/cluster/cluster.cpp` (from about `:225`), `cluster.hpp` (from `:122`), `net.cpp` (one line moves at `:2304`, a net shift of one after it) and comments in `src/types.hpp` and `src/config.hpp` changed, so those line numbers are off by a few, not re-cited. Read a copy at `build/scratch/macha-cac5998`.

Status: specification, 2026-11-05. Nothing here is built. The first of two
cutovers: Macha runs on Dannan's transport and node with Macha's own store, in
one flag day, then a soak. Macha's code is the source of truth: where it and a
design document differ, Macha's behaviour is the plan.

Rulings that stand: Dannan's wire v1 and v2 and the flag day; adverts in place of
`NodeInfo` fields; the hand-written UPnP; Dannan's own state-file formats with a
one-shot importer; Macha's message numbers re-registered from 512, payloads
unchanged.

Verified against Macha `00861dd` (0.92.3), extracted at
`build/scratch/macha-00861dd`, and Dannan's tree on `feature/macha-parity` at
`4742cb4`. Macha citations are relative to its `src/`; Dannan's to `dannan/`. The
Status, message and journal tables were generated from those sources by
`build/scratch/replan/gen_tables.py`, so a row's line is the line. The proposals
(advert names, hosted ids, numbers) are marked as such. Findings are `F1` to `F22`,
ranked, in section 8.

The stale inputs. The extraction proposal's file table still says what goes where,
but its "`telemetry.hpp`'s record keeps its storage, cache and playback fields" is
superseded by hosted fields. Its seam-1 table places keys in a `dannan::Config` that
does not exist in that shape: `NodeConfig` has no `log_level`, `plugin_path`,
`runtime.glibc_arena_max` or `service_startup_*` (section 3). Parity item 4's reading
of `RecoveryProgress` is wrong in one respect (F5), and the evaluation's rollback line
is wrong (F6).

## 1. What Dannan replaces

### 1.1 File table

Lines are from the two trees as they stand (implementation plus header).

| Macha | Lines | Dannan piece | Lines | What stays in Macha |
|---|---|---|---|---|
| `cluster/net.{cpp,hpp}` | 4718 + 737 | `net/channel` 732, `client` 1711, `session` 749, `server` 383, `scheduler` 517, `assembler` 133, `outbound` 320, `message` 112 | 4657 | `MessageType` with its numbers, `message_type_name`, `frame_type_name` (spelled `read-ahead`), `default_frame_type`; `AsyncRpc` as a wrapper over `CallHandle`; `RpcMessage`, `RpcReply`, `RpcFrame` as adapters; `have_valid_objects_max`, `max_pending_rpc_requests` |
| `cluster/message_routes.hpp` | 69 | `net/routes` 261; `Routes` in `context.h:142-169`; `Node::handle`, `node.cpp:677-731` | 261 | the bind sites, wrapping each handler to return `net::Handled`; the 128-slot table goes |
| `cluster/frame_type.hpp` | 48 | `sched/work.h` (`FrameType`, `WorkClass`) | 193 | the word `viewer` for `WorkClass::foreground`, as an alias; values are equal, 1 to 5 |
| `cluster/rpc_refusal.hpp` | 66 | `net/message` (`error_reply`, `refusal_of`, `try_later`) | 112, counted above | nothing: byte for byte the same, `net/message.cpp:12-48` |
| `cluster/membership.{cpp,hpp}` | 395 + 83 | `membership/membership` 487, `node_record` 194, `identity_reset` 119 | 800 | a `NodeInfo` view over `NodeRecord` (capacity, used, generation and flags from adverts); `Membership::storage`, `usage`, `metadata_generation`, `hosts_extents` as facade calls |
| `cluster/placement.{cpp,hpp}` | 422 + 37 | `placement/placement` | 403 | the salts (`macha/data-fallback/v7`, `macha/data-extra-domain/v7`), the capacity advert's name, the `hosts_extents` filter. Parity item 9 checked 2,704 orders against Macha's reference |
| `cluster/node_identity.{cpp,hpp}` | 48 + 23 | `identity/node_identity` 110, `node_id` 79, `cluster_key` 79 | 268 | `load_v18_node_id`'s layout marker (`node_identity.cpp:15-40`) and `ClusterKeys` (F9) |
| `cluster/public_connectivity.{cpp,hpp}`, `miniupnpc_compat.hpp` | 513 + 88 + 20 | `reach/connectivity` 424, `upnp` 416, `external_ip` 110 | 950 | the Status JSON. The inbound half of `cluster.cpp` goes to `reach/inbound` 219 and `dial_back` 119 |
| `cluster/activity_clocks.{cpp,hpp}` | 51 + 49 | `status/activity` | 90 | see F8: Macha keeps its own instance for this cutover |
| `cluster/node_events.hpp` | 62 | `status/events` | 104 | the kinds `storage`, `metadata`, `claims` named at start; the mapping of F7 |
| `cluster/telemetry.{cpp,hpp}` | 778 + 235 | `status/telemetry` | 706 | `NodeTelemetry` as a view decoded from the record; the hosted fields (section 6); `PlaybackBudgets`, `parse_release_version`, `runs_at_least`, `CacheActivity` |
| `subsystem/*` | 596 (`supervisor` 320 + 68, `subsystem` 18 + 83, `registry` 38 + 38, `abi` 31) | `supervise/supervisor` 300, `subsystem.h` 79, `registry.h` 58 | 437 | `SubsystemContext`'s contents, passed as `std::any host`, `subsystem.h:36-42`; the typed registry (`fuse()`); the entry symbol and `kBuildIdentity` through `SupervisorOptions`. Plugins rebuild against `PluginEntry` (F22) |
| `contract/work.{cpp,hpp}` | 59 + 118 | `sched/work` | 257 | each caller's declared waits |
| `retained_memory.hpp` | 402 | `sched/retained_memory` 387, `memory_limit` 75 | 462 | the eleven owners, as `hosted_first + n` (section 2) and through `Node::memory()`: `Budget::reserve` charges only `hosted_first`, `node.cpp:262` |
| `retry_policy.hpp` | 97 | `sched/retry.h` | 92 | nothing; defaults equal |
| `log.{cpp,hpp}` | 109 + 113 | `status/log` | 248 | `Log::` and a `LogSink` over it, so the line format stays Macha's |
| `durable_file.{cpp,hpp}` | 81 + 14 | `identity/durable_file` | 160 | nothing |
| `observation.{cpp,hpp}` | 354 + 241 | `status/observation` | 568 | the gauges, through `NodeConfig::observation_gauges` |
| `supervised.{cpp,hpp}` | 181 + 62 | `supervise/supervised` | 172 | every Macha thread's name; the process-wide registry until each thread takes `Context::threads()` (F14) |
| `startup_progress.hpp` | 67 | `supervise/startup` | 154 | `Service` starts `StartupWatch`, parity item 3 |

### 1.2 The `NodeRuntime` split

Every public method of `cluster/cluster.hpp:178-292`. "Node" is `dannan::Node`
beneath the Context, "Facade" a Macha class over it, "Local" Macha's local services.

| Method | Goes to | Operation |
|---|---|---|
| constructor (`:181`) | Node, Facade | `Node(io, NodeConfig)`; the facade resolves the config first (F3, F4) and registers every type before start |
| `start` (`:185`) | Node, Facade | `Node::start()`; the facade keeps `configuration_warnings`, the two start lines and the stage hook, `cluster.cpp:368-381` |
| `request_stop` (`:186`) | **Gap** | `Node` has `stop()` only. `Service::request_stop` calls it from `service.cpp:603`, `:623` to end a startup waiting on recovery. F9 |
| `cancel_outbound_calls` (`:189`) | Node | `Node::cancel_outbound_calls()` |
| `stop` (`:190`) | Node | `Node::stop()`; same order: server, then client, then loops |
| `advertise_storage` (`:194`) | Facade | `Nodes::advertise` of `macha/capacity`, `macha/used`; the figures also go to hosted 1024, 1025 |
| `advertise_storage_backends` (`:195`) | Facade | an atomic read by `NodeConfig::telemetry_fields` |
| `advertise_metadata_generation` (`:196`) | Facade | `Nodes::advertise` of `macha/metadata-generation`, held at its maximum as `membership.cpp:200` does; hosted 1028 |
| `advertise_cache` (`:197`) | Facade | atomics read by `telemetry_fields` |
| `wait_local_state_ready` (`:198`) | Facade | `Readiness::wait_complete`, keeping `cluster.cpp:355`'s not-started rule |
| `readiness` (`:199`) | Facade | `Readiness` by plane name; `local_state_ready` is `complete() && !failed()`; F5 |
| `config` (`:200`) | Facade | Macha's `Config` |
| `keys` (`:203`) | Facade | Macha's own `load_cluster_keys(key_file)`. `users.cpp:607` and `:636` seal with the master itself, and `Identity::derive_key` never hands it out. `derive_key("macha/storage/v1")` would equal `keys.storage`: same SHA-256 of the same trimmed file (`crypto.cpp:107-113`, `identity/cluster_key.cpp:26-32`), same HKDF. The disk stays readable either way |
| `node_id` (`:206`) | Node | `Identity::id()` |
| `durability_epoch` (`:209`) | Node | `Identity::durability_epoch()` |
| `publish_self` (`:214`) | Facade | nothing to do: `ServerOptions::local` reads the roster at each handshake, `node.cpp:472` |
| `signal_telemetry_refresh` (`:217`) | Node | `Telemetry::refresh()` |
| `membership` (`:218`, `:221`) | Facade | a `Membership` view over `Nodes`: `self`, `all`, `active`, `snapshot`, `directly_reachable`, `all_known_reachable`, `inbound_capable`, `hosts_extents`; `forget_unseen` runs in the node's loop. F19 for `snapshot` |
| `telemetry` (`:224`, `:227`) | Facade | a `TelemetryStore` view over `Telemetry`, decoding `NodeTelemetry` per record |
| `broadcast_best_effort` (`:232`) | Node | `Channels::broadcast_best_effort` |
| `broadcast_notify` (`:236`) | Node | `Channels::notify` |
| `call`, four forms (`:239-242`) | Facade over Node | `Routes::call` with Macha's `default_frame_type` and the `wait_for_reply` loop of `net.cpp:2513-2546` over `CallHandle::idle_for`. F1, F2 |
| `call_async`, four forms (`:243-247`) | Facade over Node | `Routes::call`, a promise and the `CallHandle`, as `AsyncRpc` (`cancel`, `abort`, `promote`, `idle_for`). F1, F2 |
| `announce_metadata_generation` (`:250`) | Facade | epoch and count bumps, `Nodes::advertise`, `Channels::broadcast` of `metadata_notice` |
| `reconfigure_local` (`:251`) | Facade | Macha's config only, `cluster.cpp:1148-1151` |
| `traffic_totals` (`:253`) | Node | `Diagnostics::traffic`; six classes, indexed alike |
| `peer_viewers_active` (`:257`) | Facade | over `Telemetry::views`, section 6 |
| `remote_metadata_generation` (`:258`) | Facade | kept by the facade from `Nodes::watch`, with `settle_remote_generation`. F7 |
| `remote_metadata_epoch` (`:261`) | Facade | the same, and the `metadata_notice` handler |
| `metadata_announcements` (`:264`) | Facade | a counter |
| `apply_identity_reset` (`:267`) | Node | `Nodes::apply_identity_reset` |
| `propagate_identity_reset` (`:268`) | Node | `Nodes::propagate_identity_reset` with a deadline Macha names; it had none |
| `identity_resets` (`:269`) | Node | `Nodes::identity_resets` |
| `public_connectivity_status` (`:272`) | Node | `Diagnostics::connectivity`; optional before start, where Macha's always answers |
| `refresh_public_connectivity` (`:273`) | Node | `Nodes::refresh_public_connectivity`, waited for by the facade |
| `inbound_resolution` (`:274`) | Facade | `Diagnostics::inbound` plus the two modes and the resolved `hosts_extents` |
| `inbound_capable` (`:275`) | Node | `Diagnostics::inbound.capable` |
| `hosts_extents` (`:278`) | Facade | `resolve_hosts_extents`, `cluster.cpp:51-61`, stays Macha's |
| `rpc_stats` (`:281`) | **Gap in part** | `Diagnostics::client` has no `canonical_connections`. F15 |
| `peer_latency` (`:284`) | Node | `Nodes::latency` |
| `peer_latencies` (`:287`) | Node | `Nodes::latencies` |
| `rpc_server_work_stats` (`:290`) | Node | `Diagnostics::work`, section 2 |

Free functions stay with Macha: `advertised_api_endpoint` and
`enforced_playback_budgets`, `cluster.hpp:68`, `:73`. Nothing goes to Macha's local
services (`LocalServices`, `LocalState`): they already own what the node did not,
and they call the facade. `RecoveryProgress` is the only piece they touch, and it
becomes `Readiness`.

Two gaps: `request_stop` has no home, and `rpc_stats` has one in part.

## 2. Status surface

Every key of `GET /api/v1/status` (`status_response`), `/status/nodes` (the same,
with a page of nodes and `next_cursor`), `/status/nodes/{id}` (one `nodes[]` object),
`/status/diagnostics` (`diagnostics_response`) and the two `POST .../connectivity/check`
forms, from `api/status_api.cpp`. Lines are in that file. The object's keys are
sorted on output (`Json::Object` is a `std::map`, `json.hpp:18`), so renumbering a
type changes no order.

The sources after the cutover:

- `Diagnostics` is `Context::diagnostics()`, `context.h:219-230`.
- "core N" is a field of Dannan's telemetry record, `status/telemetry.h:41-62`.
- "hosted N" is the proposal of section 6.
- Adverts are proposals: `macha/capacity` and `macha/used` (u64 by `u64_advert`),
  `macha/metadata-generation` (u64), `macha/metadata-write-replicas` (u32),
  `macha/hosts-extents` (present or absent, since `placement::with_advert` tests
  presence, `placement.h:79`).

The retained-memory owners are also a proposal: Dannan's `rpc_frame` keeps its id 1,
and Macha's other ten number from `MemoryOwner::hosted_first`, 256.

One key has no source; see the Mark column. The rest render byte-identically once
the marked findings are closed. Volatile keys are masked in the golden as before.

**`root`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `node_id` | :842 | `node_.node_id()` | `Identity::id()` |  |
| `cluster` | :843 | object | object |  |
| `startup` | :844 | object | object |  |
| `nodes` | :847 | array over known nodes | array, same merge over the facade's roster and telemetry |  |
| `connectivity` | :851 | object | object |  |
| `subsystems` | :865 | object | array |  |
| `threads` | :882 | array | array |  |
| `diagnostics_endpoint` | :884 | constant | Macha's own |  |
| `generated_at_unix_ms` | :885 | `unix_ms()` | Macha's own (volatile) |  |
| `diagnostics` | :1442 | object | object |  |

**`root (GET /status/nodes only)`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `next_cursor` | `paging.cpp:71` | `page_json` | Macha's own |  |

**`cluster`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `health` | :779 | computed from roster and telemetry | Macha's own over the facade's roster and hosted fields |  |
| `conditions` | :780 | computed from roster and telemetry | Macha's own over the facade's roster and hosted fields |  |
| `nodes_known` | :781 | computed from roster and telemetry | Macha's own over the facade's roster and hosted fields |  |
| `nodes_online` | :782 | computed from roster and telemetry | Macha's own over the facade's roster and hosted fields |  |
| `nodes_hosting_extents` | :783 | computed from roster and telemetry | Macha's own over the facade's roster and hosted fields |  |
| `nodes_hosting_extents_online` | :784 | computed from roster and telemetry | Macha's own over the facade's roster and hosted fields |  |
| `nodes_inbound_incapable` | :785 | computed from roster and telemetry | Macha's own over the facade's roster and hosted fields |  |
| `metadata_generation` | :786 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_replicas` | :788 | computed from `known` | Macha's own over the facade's roster |  |
| `metadata_replicas_online` | :789 | computed from `known` | Macha's own over the facade's roster |  |
| `metadata_min_write_replicas` | :790 | computed from `known` | Macha's own over the facade's roster |  |
| `metadata_voters` | :793 | computed from `known` | Macha's own over the facade's roster |  |
| `metadata_voters_online` | :794 | computed from `known` | Macha's own over the facade's roster |  |
| `metadata_quorum_required` | :795 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_availability` | :796 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_read_available` | :797 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_quorum_available` | :798 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_write_available` | :799 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_replica_set_validated` | :800 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_quorum_validated` | :801 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_replica_set_validated_at_unix_ms` | :802 | `MetadataManager`, `Config` | Macha's own |  |
| `metadata_quorum_validated_at_unix_ms` | :803 | `MetadataManager`, `Config` | Macha's own |  |
| `storage_known` | :805 | object | object |  |
| `storage_online` | :807 | object | object |  |
| `cache_known` | :809 | object | object |  |
| `cache_online` | :812 | object | object |  |

**`cluster.storage_known`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :70 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `capacity_bytes` | :71 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `used_bytes` | :72 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `free_bytes` | :73 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |

**`cluster.storage_online`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :70 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `capacity_bytes` | :71 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `used_bytes` | :72 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `free_bytes` | :73 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |

**`cluster.cache_known`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :70 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `capacity_bytes` | :71 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `used_bytes` | :72 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `free_bytes` | :73 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |

**`cluster.cache_online`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :70 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `capacity_bytes` | :71 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `used_bytes` | :72 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |
| `free_bytes` | :73 | rollup of nodes | Macha's own over hosted 1024 to 1027 |  |

**`startup`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `phase` | :817 | `readiness()` and `metadata_manager` | Macha's own over `Readiness`; needs the sixth plane, F5 | F5 |
| `control_plane` | :822 | `control_plane_online_` | `Readiness::control_plane_online()` |  |
| `api` | :823 | constant | Macha's own |  |
| `data_storage` | :824 | `RecoveryProgress` | `Readiness::has("data_storage")` |  |
| `control_storage` | :825 | `RecoveryProgress` | `Readiness::has("control_storage")` |  |
| `cache` | :826 | `RecoveryProgress` | `Readiness::has("cache")` |  |
| `retention` | :827 | `RecoveryProgress` | `Readiness::has("retention")` |  |
| `metadata` | :828 | `RecoveryProgress` | `Readiness::has("metadata")` |  |
| `services` | :829 | `metadata_manager` present | Macha's own |  |
| `started_at_unix_ms` | :830 | `startup_unix_ms_` | `Diagnostics` `.started_unix_ms` |  |
| `ready_at_unix_ms` | :831 | `mark_complete` time | `Readiness::ready_unix_ms()`; needs the sixth plane, F5 | F5 |
| `error_code` | :833 | `readiness.error` present | Macha's own over `Readiness::error()` |  |
| `error` | :834 | first failure | `Readiness::error()`; holds the last, not the first, F5 | F5 |

**`nodes[]`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `id` | :226 | `known` map key | `NodeRecord::id` or telemetry field 1 |  |
| `state` | :227 | `active_members`, the reset | Macha's own over `Nodes::snapshot()` |  |
| `inbound_capable` | :231 | `NodeInfo::flags` bit 0 | `NodeRecord::inbound_capable()` |  |
| `hosts_extents` | :232 | `NodeInfo::flags` bit 1 | advert `macha/hosts-extents`, by presence |  |
| `dialable` | :234 | `NodeInfo::flags` bit 0 | `NodeRecord::inbound_capable()` |  |
| `inbound_capable_mode` | :241 | `Config` | Macha's own |  |
| `hosts_extents_mode` | :243 | `Config` | Macha's own |  |
| `telemetry_freshness` | :246 | `TelemetryView`, `known` | Macha's own over `Telemetry::views` and `persisted` |  |
| `phase` | :251 | `NodeTelemetry::phase` | Telemetry core 18, `status::phase_name` (same words) |  |
| `observed_at_unix_ms` | :252 | telemetry, durable, roster | Telemetry core 4; `NodeRecord::seen_unix_ms` |  |
| `live_age_ms` | :256 | `TelemetryView::age` | `status::TelemetryView::age` |  |
| `version` | :257 | `NodeTelemetry::version` | Telemetry core 5 (`NodeConfig::version` must be `kServerVersion`, F11) |  |
| `node_name` | :261 | `NodeTelemetry::node_name` | Telemetry core 21 |  |
| `host` | :263 | roster, telemetry | `NodeRecord::host`; core 6 |  |
| `port` | :264 | roster, telemetry | `NodeRecord::port`; core 8 |  |
| `failure_domain` | :266 | roster, telemetry | `NodeRecord::failure_domain`; core 7 |  |
| `metadata_generation` | :275 | `NodeInfo`, `NodeTelemetry` | advert `macha/metadata-generation`; hosted 1028 |  |
| `api_endpoint` | :282 | `NodeTelemetry::api_endpoint` | hosted 1030 |  |
| `storage` | :289 | object | object |  |
| `cache` | :307 | object | object |  |
| `storage_backends_online` | :309 | `NodeTelemetry` | hosted 1029 |  |
| `roles` | :319 | computed | Macha's own over hosted 1024 and 1026, advert `macha/capacity` |  |
| `runtime` | :345 | object | object |  |
| `traffic` | :374 | object or null | object or null |  |
| `playback` | :449 | object | object |  |
| `identity_association_reset` | :450 | resets merged with metadata | Macha's own merge over `Nodes::identity_resets()` |  |

**`nodes[].storage`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :70 | telemetry known | Macha's own over hosted 1024, 1025 |  |
| `capacity_bytes` | :71 | telemetry or `NodeInfo::capacity` | hosted 1024; advert `macha/capacity` |  |
| `used_bytes` | :72 | telemetry | hosted 1025 |  |
| `free_bytes` | :73 | computed | Macha's own |  |

**`nodes[].cache`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `hits` | :303 | `NodeTelemetry` | hosted 1038 |  |
| `misses` | :304 | `NodeTelemetry` | hosted 1039 |  |
| `evictions` | :305 | `NodeTelemetry` | hosted 1040 |  |
| `available` | :70 | telemetry known | Macha's own over hosted 1026, 1027 |  |
| `capacity_bytes` | :71 | telemetry | hosted 1026 |  |
| `used_bytes` | :72 | telemetry | hosted 1027 |  |
| `free_bytes` | :73 | computed | Macha's own |  |

**`nodes[].runtime`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `uptime_ms` | :327 | `NodeTelemetry` | Telemetry core 9 |  |
| `rss_bytes` | :328 | `NodeTelemetry` | Telemetry core 10 |  |
| `process_cpu_percent` | :329 | `NodeTelemetry` | Telemetry core 11, / 1000 |  |
| `load1` | :331 | `NodeTelemetry` | Telemetry core 12, / 1000 |  |
| `cpu_cores` | :335 | `NodeTelemetry` | Telemetry core 19 |  |
| `memory_total_bytes` | :338 | `NodeTelemetry` | Telemetry core 20 |  |
| `peers_known` | :339 | `NodeTelemetry` | Telemetry core 13 |  |
| `peers_active` | :340 | `NodeTelemetry` | Telemetry core 14 |  |
| `rpc_connections_created` | :341 | always 0, `cluster.cpp:949` | Telemetry core 15, always 0, `node.cpp:954` |  |
| `rpc_connections_reused` | :342 | always 0, `cluster.cpp:949` | Telemetry core 16, always 0 |  |
| `rpc_connections_canonical` | :343 | peers_active - 1 | Telemetry core 17, peers_active - 1 |  |

**`nodes[].traffic`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `as_of_unix_ms` | :375 | `observed_unix_ms` | Telemetry core 4 |  |
| `window_ms` | :376 | `traffic_window_ms` | `Telemetry::traffic_window_ms()`, core 22 |  |
| `classes` | :378 | `NodeTelemetry::traffic` | `Telemetry::traffic()`, core 22 |  |

**`nodes[].traffic.classes[]`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `class` | :366 | `FrameType`, status_api names | Macha's own; the names (`read_ahead`) are status_api's, not `sched::frame_type_name` |  |
| `in_bytes` | :367 | `TrafficClass` | `status::TrafficClass`, core 22 |  |
| `out_bytes` | :368 | `TrafficClass` | `status::TrafficClass`, core 22 |  |
| `in_bytes_per_s` | :369 | `TrafficClass` | `status::TrafficClass`, core 22 |  |
| `out_bytes_per_s` | :371 | `TrafficClass` | `status::TrafficClass`, core 22 |  |

**`nodes[].playback`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `startup_timeout_ms` | :391 | `NodeTelemetry` | hosted 1031 |  |
| `segment_timeout_ms` | :394 | `NodeTelemetry` | hosted 1032 |  |
| `pipeline_idle_ms` | :397 | `NodeTelemetry` | hosted 1033 |  |
| `session_idle_ms` | :400 | `NodeTelemetry` | hosted 1034 |  |
| `max_sessions_per_account` | :406 | `NodeTelemetry` | hosted 1035 |  |
| `max_transcodes_per_account` | :409 | `NodeTelemetry` | hosted 1041 |  |
| `max_sessions` | :414 | `NodeTelemetry` | hosted 1036 |  |
| `transcode_entitlement_idle_ms` | :419 | `NodeTelemetry` | hosted 1037 |  |
| `startup_no_progress_ms` | :423 | `NodeTelemetry` | hosted 1042 |  |
| `start_wait_max_ms` | :426 | `NodeTelemetry` | hosted 1043 |  |
| `start_failed_retention_ms` | :428 | `NodeTelemetry` | hosted 1044 |  |
| `transcode_rates` | :446 | `NodeTelemetry` | hosted 1045 |  |

**`nodes[].playback.transcode_rates[]`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `bit_depth` | :441 | `TranscodeRate` | decoded from hosted 1045 |  |
| `height_class` | :442 | `TranscodeRate` | decoded from hosted 1045 |  |
| `kind` | :435 | `TranscodeRate` | decoded from hosted 1045 |  |
| `codec` | :436 | `TranscodeRate` | decoded from hosted 1045 |  |
| `rate` | :437 | `TranscodeRate` | decoded from hosted 1045 |  |
| `observations` | :438 | `TranscodeRate` | decoded from hosted 1045 |  |
| `concurrent` | :439 | `TranscodeRate` | decoded from hosted 1045 |  |

**`nodes[].identity_association_reset`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `scope` | :172 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |
| `host` | :173 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |
| `port` | :174 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |
| `stale_node_id` | :175 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |
| `epoch` | :177 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |
| `reset_at_unix_ms` | :178 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |
| `reset_by_node_id` | :179 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |
| `reason` | :180 | `IdentityAssociationReset`, `identity_reset_key` | `membership::IdentityReset`; `key()` gives the same text |  |

**`connectivity`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `configured` | :141 | `PublicConnectivityStatus` | `Diagnostics` `.connectivity->configured` |  |
| `advertised` | :142 | `PublicConnectivityStatus` | `Diagnostics` `.connectivity->advertised` |  |
| `upnp` | :143 | object | object |  |
| `external_ip` | :144 | object | object |  |
| `check` | :145 | object | object |  |
| `inbound_capable` | :152 | `inbound_resolution()` | `Diagnostics` `.inbound.capable` |  |
| `inbound_capable_mode` | :153 | `Config`, `tristate_name` | Macha's own ("true", "false", "auto"; `tristate_name` stays) |  |
| `inbound_capable_source` | :154 | `inbound_resolution()` | `Diagnostics` `.inbound.source` |  |
| `inbound_capable_decided_at_unix_ms` | :155 | `inbound_resolution()` | `Diagnostics` `.inbound.decided_unix_ms` |  |
| `hosts_extents` | :157 | `inbound_resolution()` | Macha's own: the facade resolves it from the mode, backends and `.inbound.capable` |  |
| `hosts_extents_mode` | :158 | `Config` | Macha's own |  |

**`connectivity.configured`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `host` | :94 | status | `Diagnostics` `.connectivity->configured.host` |  |
| `port` | :94 | status | `Diagnostics` `.connectivity->configured.port` |  |

**`connectivity.advertised`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `host` | :94 | status | `Diagnostics` `.connectivity->advertised.host` |  |
| `port` | :94 | status | `Diagnostics` `.connectivity->advertised.port` |  |
| `source` | :96 | status | `Diagnostics` `.connectivity->advertised_source` |  |

**`connectivity.upnp`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `enabled` | :102 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.enabled` |  |
| `support_built` | :103 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.support_built` |  |
| `gateway_found` | :104 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.gateway_found` |  |
| `mapping_active` | :105 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.mapping_active` |  |
| `mapping_created` | :106 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.mapping_created` |  |
| `mapping_owned` | :107 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.mapping_owned` |  |
| `private_wan` | :108 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.private_wan` |  |
| `lan_address` | :109 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.lan_address` |  |
| `external_address` | :111 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.external_address` |  |
| `internal_port` | :113 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.internal_port` |  |
| `external_port` | :114 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.external_port` |  |
| `lease_seconds` | :115 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.lease_seconds` |  |
| `igd_status` | :116 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.igd_status` |  |
| `error_code` | :117 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.error_code` |  |
| `error` | :118 | `PublicConnectivityStatus::upnp` | `Diagnostics` `.connectivity->upnp.error` |  |

**`connectivity.external_ip`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `enabled` | :121 | `PublicConnectivityStatus::external_ip` | `Diagnostics` `.connectivity->external_ip.enabled` |  |
| `attempted` | :122 | `PublicConnectivityStatus::external_ip` | `Diagnostics` `.connectivity->external_ip.attempted` |  |
| `address` | :123 | `PublicConnectivityStatus::external_ip` | `Diagnostics` `.connectivity->external_ip.address` |  |
| `error_code` | :125 | `PublicConnectivityStatus::external_ip` | `Diagnostics` `.connectivity->external_ip.error_code` |  |
| `error` | :127 | `PublicConnectivityStatus::external_ip` | `Diagnostics` `.connectivity->external_ip.error` |  |

**`connectivity.check`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `enabled` | :131 | status | `Diagnostics` `.connectivity->check_enabled` |  |
| `self_probe` | :132 | status | `Diagnostics` `.connectivity->self_probe` |  |
| `error` | :133 | status | `Diagnostics` `.connectivity->self_probe_error` |  |
| `checked_at_unix_ms` | :135 | status | `Diagnostics` `.connectivity->checked_unix_ms` |  |
| `externally_verified` | :138 | constant false | Macha's own |  |

**`connectivity.dial_back`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `last_probe_unix_ms` | :160 | `inbound_resolution()` | `Diagnostics` `.inbound.last_probe_unix_ms` |  |
| `last_probe_peer` | :162 | `inbound_resolution()` | `Diagnostics` `.inbound.last_probe_peer` |  |
| `last_probe_error` | :164 | `inbound_resolution()` | `Diagnostics` `.inbound.last_probe_error` |  |
| `consecutive_failures` | :166 | `inbound_resolution()` | `Diagnostics` `.inbound.consecutive_failures` |  |

**`subsystems[]`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `name` | :858 | `SubsystemSupervisor` | `supervise::Supervisor::statuses()` |  |
| `state` | :859 | `subsystem_state_name` | `supervise::state_name` (same six words) |  |
| `restart_count` | :860 | `SubsystemStatus` | `supervise::SubsystemStatus` |  |
| `last_fault` | :861 | `SubsystemStatus` | `supervise::SubsystemStatus` |  |

**`threads[]`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `name` | :871 | process-wide registry | `Diagnostics` `.threads` merged with Macha's; names change, F14 | F14 |
| `running` | :872 | registry | `Diagnostics` `.threads` |  |
| `restarting` | :873 | registry | `Diagnostics` `.threads` |  |
| `faults` | :874 | registry | `Diagnostics` `.threads` |  |
| `last_fault_code` | :875 | registry | `Diagnostics` `.threads` |  |
| `last_fault` | :877 | registry | `Diagnostics` `.threads` |  |
| `last_fault_unix_ms` | :878 | registry | `Diagnostics` `.threads` |  |

**`diagnostics`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `metadata` | :972 | object | object |  |
| `rpc_server` | :994 | object | object |  |
| `rpc_transport` | :1007 | object | object |  |
| `data_resources` | :1037 | object | object |  |
| `retained_memory` | :1079 | object | object |  |
| `data_store` | :1104 | object | object |  |
| `filesystem` | :1290 | object | object |  |
| `convergence` | :1309 | object | object |  |
| `prompt_replication` | :1375 | object | object |  |
| `repair` | :1380 | object | object |  |
| `http` | :1421 | object | object |  |
| `auth` | :1439 | object | object |  |

**`diagnostics.metadata`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :909 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `historical_requests` | :912 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `historical_reconstructions` | :913 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `historical_deltas_applied` | :914 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `materialization_cache_hits` | :915 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `materialization_cache_misses` | :916 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `materialization_cache_evictions` | :917 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `materialization_cache_entries` | :919 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `materialization_cache_bytes` | :921 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `materialization_cache_limit_bytes` | :923 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `history_records` | :925 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `history_file_bytes` | :926 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `history_resident_payload_bytes` | :927 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `accepted_head_persistence_writes` | :929 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `accepted_head_persistence_bytes` | :931 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `accepted_head_persistence_failures` | :933 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `conflicts` | :947 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `namespace_conflicts` | :948 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `catalogue_conflicts` | :949 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `tombstones` | :950 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `conflicts_superseded` | :953 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `conflicts_resolved` | :954 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `head_holders` | :960 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `head_holders_present` | :961 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `heads_set_aside` | :962 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `mutations` | :966 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `mutation_retention_ms_total` | :967 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `mutation_retention_ms_max` | :968 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `mutation_publish_ms_total` | :969 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |
| `mutation_publish_ms_max` | :970 | `LocalServices`, `MetadataManager` | Macha's own: unchanged |  |

**`diagnostics.rpc_server`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `metadata_pending_jobs` | :976 | `rpc_server_work_stats()` | `Diagnostics` `.work.ordered_pending_jobs` |  |
| `metadata_pending_bytes` | :977 | `rpc_server_work_stats()` | `Diagnostics` `.work.ordered_pending_bytes` |  |
| `metadata_active_jobs` | :978 | `rpc_server_work_stats()` | `Diagnostics` `.work.ordered_active_jobs` |  |
| `metadata_rejected_jobs` | :979 | `rpc_server_work_stats()` | `Diagnostics` `.work.ordered_rejected` |  |
| `fast_control_pending_bytes` | :980 | `rpc_server_work_stats()` | `Diagnostics` `.work.fast_control_pending_bytes` |  |
| `control_pending_bytes` | :982 | `rpc_server_work_stats()` | `Diagnostics` `.work.control_pending_bytes` |  |
| `data_pending_bytes` | :984 | `rpc_server_work_stats()` | `Diagnostics` `.work.data_pending_bytes` |  |
| `rejected_jobs` | :985 | `rpc_server_work_stats()` | `Diagnostics` `.work.rejected` |  |
| `frame_timings` | :989 | keyed by `frame_type_name`: `read-ahead` | `Diagnostics` `.work.frame_timings`; keep Macha's spelling, `sched::frame_type_name` says `read_ahead`, F14 | F14 |
| `message_timings` | :993 | keyed by `message_type_name` | `Diagnostics` `.work.message_timings`, `Registration::name`; `dial_back_probe` is core `dial_back`, F14 | F14 |

**`diagnostics.rpc_server.frame_timings.<frame>`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `requests` | :85 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `queue_wait_us_total` | :86 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `queue_wait_us_max` | :87 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `handler_us_total` | :88 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `handler_us_max` | :89 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |

**`diagnostics.rpc_server.message_timings.<type>`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `requests` | :85 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `queue_wait_us_total` | :86 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `queue_wait_us_max` | :87 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `handler_us_total` | :88 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |
| `handler_us_max` | :89 | `RpcServerWorkStats::Timing` | `net::Timing`, same five figures |  |

**`diagnostics.rpc_transport`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `connections_created` | :998 | `RpcStats`: dials that handshook, `net.cpp:2304` | `Diagnostics` `.client.dials - dial_failures`, near but not equal, F15 | F15 |
| `connections_reused` | :999 | `RpcStats` | `Diagnostics` `.client.reused`; fewer sites count it | F15 |
| `peer_latency_ms` | :1005 | `peer_latencies()` | `Nodes::latencies()` |  |
| `canonical_connections` | :1006 | `RpcStats`: usable routes, `net.cpp:3004-3009` | **no source**: `ClientStats` has no such count | GAP F15 |

**`diagnostics.data_resources`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `capacity_bytes` | :1011 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `viewer_reserve_bytes` | :1012 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `used_bytes` | :1013 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `peak_used_bytes` | :1014 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `viewer_admissions` | :1015 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `loader_admissions` | :1016 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `speculative_admissions` | :1017 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `viewer_waits` | :1019 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `loader_waits` | :1020 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `speculative_waits` | :1021 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `background_limit` | :1023 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `background_active` | :1024 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `peak_background_active` | :1025 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `device_pressured` | :1029 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `device_service_us` | :1030 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `device_worst_us` | :1031 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `device_slowdown_percent` | :1032 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `device_pressure_onsets` | :1033 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `pressure_refusals` | :1035 | `DataResourceArbiter` | Macha's own: unchanged |  |
| `cancelled_waits` | :1036 | `DataResourceArbiter` | Macha's own: unchanged |  |

**`diagnostics.retained_memory`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `capacity_bytes` | :1041 | `RetainedMemoryStats` | `Node::memory().stats().capacity_bytes` |  |
| `control_reserve_bytes` | :1042 | `RetainedMemoryStats` | `Node::memory().stats().control_reserve_bytes` |  |
| `viewer_reserve_bytes` | :1044 | `RetainedMemoryStats` | `Node::memory().stats().foreground_reserve_bytes` |  |
| `loader_reserve_bytes` | :1046 | `RetainedMemoryStats` | `Node::memory().stats().loader_reserve_bytes` |  |
| `reassembly_reserve_bytes` | :1050 | `RetainedMemoryStats` | `Node::memory().stats().reassembly_reserve_bytes` |  |
| `used_bytes` | :1052 | `RetainedMemoryStats` | `Node::memory().stats().used_bytes` |  |
| `peak_used_bytes` | :1053 | `RetainedMemoryStats` | `Node::memory().stats().peak_used_bytes` |  |
| `reclaimable_bytes` | :1054 | `RetainedMemoryStats` | `Node::memory().stats().reclaimable_bytes` |  |
| `shed_requests` | :1055 | `RetainedMemoryStats` | `Node::memory().stats().shed_requests` |  |
| `cancelled_waits` | :1056 | `RetainedMemoryStats` | `Node::memory().stats().cancelled_waits` |  |
| `restored_bytes` | :1057 | `RetainedMemoryStats` | `Node::memory().stats().restored_bytes` |  |
| `overcommit_bytes` | :1058 | computed | Macha's own over `stats()` |  |
| `admissions` | :1070 | object | object |  |
| `waits` | :1071 | object | object |  |
| `owners` | :1078 | object | object |  |

**`diagnostics.retained_memory.admissions`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `control` | :1067 | `stats().admissions` | `stats().admissions[0]`, `WorkClass` order |  |
| `viewer` | :1067 | `stats().admissions` | `stats().admissions[1]`, `WorkClass` order |  |
| `loader` | :1067 | `stats().admissions` | `stats().admissions[2]`, `WorkClass` order |  |
| `speculative` | :1067 | `stats().admissions` | `stats().admissions[3]`, `WorkClass` order |  |

**`diagnostics.retained_memory.waits`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `control` | :1068 | `stats().waits` | `stats().waits[0]`, `WorkClass` order |  |
| `viewer` | :1068 | `stats().waits` | `stats().waits[1]`, `WorkClass` order |  |
| `loader` | :1068 | `stats().waits` | `stats().waits[2]`, `WorkClass` order |  |
| `speculative` | :1068 | `stats().waits` | `stats().waits[3]`, `WorkClass` order |  |

**`diagnostics.retained_memory.owners`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `fuse_request` | :1077 | `owner_bytes` | `stats().owner_bytes[256]` |  |
| `fuse_operation` | :1077 | `owner_bytes` | `stats().owner_bytes[257]` |  |
| `publication` | :1077 | `owner_bytes` | `stats().owner_bytes[258]` |  |
| `rpc_frame` | :1077 | `owner_bytes` | `stats().owner_bytes[1]` |  |
| `object_payload` | :1077 | `owner_bytes` | `stats().owner_bytes[259]` |  |
| `durability` | :1077 | `owner_bytes` | `stats().owner_bytes[260]` |  |
| `metadata` | :1077 | `owner_bytes` | `stats().owner_bytes[261]` |  |
| `catalogue` | :1077 | `owner_bytes` | `stats().owner_bytes[262]` |  |
| `media_profile` | :1077 | `owner_bytes` | `stats().owner_bytes[263]` |  |
| `playback_segment` | :1077 | `owner_bytes` | `stats().owner_bytes[264]` |  |
| `cache` | :1077 | `owner_bytes` | `stats().owner_bytes[265]` |  |

**`diagnostics.data_store`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :1082 | `LocalStore::diagnostics()` | Macha's own: unchanged |  |
| `loose_reaffirmation_fast_paths` | :1087 | `LocalStore::diagnostics()` | Macha's own: unchanged |  |
| `loose_reaffirmation_full_validations` | :1089 | `LocalStore::diagnostics()` | Macha's own: unchanged |  |
| `presence_index_entries` | :1091 | `LocalStore::diagnostics()` | Macha's own: unchanged |  |
| `pack_recovery_truncated_tails` | :1092 | `LocalStore::diagnostics()` | Macha's own: unchanged |  |
| `pack_recovery_skipped_regions` | :1094 | `LocalStore::diagnostics()` | Macha's own: unchanged |  |
| `pack_recovery_skipped_bytes` | :1096 | `LocalStore::diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.filesystem`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :1107 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `timed_out_requests` | :1114 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `merged_publications` | :1115 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_requests` | :1116 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_notifications_suppressed` | :1118 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_pressure_publication_sweeps` | :1120 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_coalesced_queued` | :1122 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_coalesced_running` | :1124 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_coalesced_unconfirmed` | :1126 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publications_started` | :1128 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publications_completed` | :1130 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_peak_active` | :1132 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_quanta` | :1134 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_yields` | :1136 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_peak_inflight_bytes` | :1138 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_pipeline_limit_bytes` | :1140 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_peak_pipeline_extents` | :1142 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_closed_priority_selections` | :1144 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_retirement_priority_selections` | :1146 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `open_publications` | :1148 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `peak_open_publications` | :1149 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `publication_max_open_writers` | :1151 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_selections_under_writer_cap` | :1153 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_progress_events` | :1155 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_bytes_read` | :1157 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_bytes_committed` | :1159 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_bytes_confirmed` | :1161 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_completed_spool_bytes_read` | :1163 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_completed_source_bytes_read` | :1165 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_completed_reused_extents` | :1167 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_completed_put_extents` | :1169 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_overlay_read_queries` | :1171 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_overlay_ranges_examined` | :1173 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_overlay_descriptors_copied` | :1175 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `retained_data_operations` | :1177 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `retained_data_operation_bytes` | :1179 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `retained_overlay_ranges` | :1181 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `retained_overlay_bytes` | :1183 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `retained_publication_operations` | :1185 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `retained_publication_operation_bytes` | :1187 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `operation_metadata_bytes` | :1189 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `peak_operation_metadata_bytes` | :1191 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `operation_metadata_limit_bytes` | :1193 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `operation_metadata_waits` | :1195 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `retained_durability_tickets` | :1197 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `data_publication_inflight_bytes` | :1199 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `backend_failures` | :1201 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `durability_batches` | :1202 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `durability_writes` | :1203 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_operations_admitted` | :1204 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_operations_recovered` | :1206 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_publication_attempts` | :1208 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_publication_batches` | :1210 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_operations_batched` | :1212 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_operations_published` | :1214 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_operations_confirmed` | :1216 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_refreshed_revision` | :1220 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `namespace_available_revision` | :1222 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `parked_publications` | :1226 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `publication_retries_backed_off` | :1227 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `publications_retrying_persistently` | :1229 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `journal_recovery_skipped_frames` | :1232 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `journal_recovery_quarantined_bytes` | :1234 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `recovery_dropped_operations` | :1236 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `publications_abandoned` | :1238 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `mountpoint_stray_entries` | :1243 | `fuse_mountpoint_preparation()` | Macha's own: unchanged |  |
| `mountpoint_immutable` | :1245 | `fuse_mountpoint_preparation()` | Macha's own: unchanged |  |
| `journal_append_batches` | :1247 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `journal_records_appended` | :1248 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `journal_durability_barriers` | :1250 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_bytes` | :1252 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_limit_bytes` | :1253 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_publish_rate_bytes_per_second` | :1254 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_publish_rate_window_bytes` | :1256 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_publish_rate_window_ms` | :1258 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_throttle_waits` | :1260 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `spool_throttle_wait_ms` | :1262 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `pending_write_request_bytes` | :1264 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `peak_pending_write_request_bytes` | :1266 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `pending_write_request_limit_bytes` | :1268 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `extent_executor_workers` | :1270 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `extent_executor_queued` | :1272 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `extent_executor_active` | :1274 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `extent_executor_peak_queued` | :1276 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `extent_executor_peak_active` | :1278 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `extent_executor_submitted` | :1280 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `inode_count` | :1282 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `peak_inode_count` | :1283 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |
| `reclaimed_inode_count` | :1284 | FUSE plugin `diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.convergence`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :1293 | `Maintenance` | Macha's own: unchanged |  |
| `events_received` | :1298 | `Maintenance` | Macha's own: unchanged |  |
| `runs_scheduled` | :1299 | `Maintenance` | Macha's own: unchanged |  |
| `runs_completed` | :1300 | `Maintenance` | Macha's own: unchanged |  |
| `requested_epoch` | :1301 | `Maintenance` | Macha's own: unchanged |  |
| `completed_epoch` | :1302 | `Maintenance` | Macha's own: unchanged |  |
| `latest_generation` | :1303 | `Maintenance` | Macha's own: unchanged |  |
| `scheduled` | :1304 | `Maintenance` | Macha's own: unchanged |  |

**`diagnostics.repair`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :1317 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `unsourceable_objects` | :1322 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `local_unreadable_objects` | :1323 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `unsourceable_sample` | :1327 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `push_examined` | :1331 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `pull_examined` | :1332 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `bytes_transferred` | :1333 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `passes_completed` | :1334 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `push_phase_complete` | :1335 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `pass_gates` | :1341 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `last_credit_bytes` | :1342 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `pace` | :1366 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `paced_by` | :1367 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.repair.pass_gates`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `ran` | :1337 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `share` | :1338 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `quiescent` | :1339 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `credit` | :1340 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.prompt_replication`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `queued` | :1370 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `copies` | :1371 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `failures` | :1372 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `skipped_no_room` | :1373 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |
| `dropped` | :1374 | `DistributedStore::repair_diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.http`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `available` | :1386 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `reactor_passes` | :1391 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `reactor_stalls` | :1392 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `reactor_longest_pass_ms` | :1393 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `connections_open` | :1394 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `connections_idle_keep_alive` | :1395 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `connections_writing` | :1397 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `connections_deferred` | :1398 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `connections_refused` | :1399 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `staged_bytes` | :1400 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `requests_served` | :1401 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `requests_deferred` | :1402 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `requests_overloaded` | :1403 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `slow_requests` | :1404 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `responses_compressed` | :1405 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `compression_bytes_saved` | :1406 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `control_lane` | :1415 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `data_lane` | :1416 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.http.control_lane`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `workers` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `busy` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `queued` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `peak_queued` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `queue_wait_ms_max` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `handled` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.http.data_lane`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `workers` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `busy` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `queued` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `peak_queued` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `queue_wait_ms_max` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |
| `handled` | :1408 | `HttpServer::diagnostics()` | Macha's own: unchanged |  |

**`diagnostics.auth`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `users` | :1428 | `Accounts`, `Config::session` | Macha's own: unchanged |  |
| `accounts_initialised` | :1429 | `Accounts`, `Config::session` | Macha's own: unchanged |  |
| `user_tombstones` | :1430 | `Accounts`, `Config::session` | Macha's own: unchanged |  |
| `user_table_hash` | :1431 | `Accounts`, `Config::session` | Macha's own: unchanged |  |
| `allow_anonymous` | :1432 | `Accounts`, `Config::session` | Macha's own: unchanged |  |
| `anonymous_roles` | :1438 | `Accounts`, `Config::session` | Macha's own: unchanged |  |

**`POST .../connectivity/check`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `results` | :1479 | roster, `call(ping)` | `Nodes::roster()`, `Routes::call_and_wait(core ping)` |  |
| `checked_at_unix_ms` | :1479 | `unix_ms()` | Macha's own |  |
| `connectivity` | :1483 | `refresh_public_connectivity(true, true)` | `Nodes::refresh_public_connectivity`, waited for by the facade |  |

**`POST .../connectivity/check: results[]`**

| Key | Line | Now | After the cutover | Mark |
|---|---|---|---|---|
| `node_id` | :1470 | roster | `NodeRecord::id` |  |
| `reachable` | :1470 | reply type `ok` | `CallOutcome` reply type `message::ok` |  |
| `error_code` | :1472 | constant `rpc_failed` | Macha's own |  |
| `error` | :1473 | exception text | `CallOutcome::failure`; wording differs |  |

## 3. Config

Keys of `config.hpp` and `config.cpp` that concern what moves. Defaults are checked
one by one against `node.h:42-134` and its nested structs; **every shared numeric
default agrees**. What differs is what Macha computes before it reaches the node.

| Key (YAML) | Macha field and default | Parsed | `NodeConfig` field, default | Finding |
|---|---|---|---|---|
| `state_path` | `state_path`, none | `config.cpp:840` | `state_path` | |
| `key_file` | `key_file`, none | `:842` | `key`, after `identity::load_cluster_key`; Macha reads the file too | same master; Macha's `keys()` stays |
| `node_name` | `node_name`, empty, at most 64 printable | `:848` | `node_name` | check stays in `config.cpp:850` |
| `network.listen` | `listen_host`, `0.0.0.0` | `:129` | `listen_host`, `0.0.0.0` | equal |
| `network.advertise` | `advertise_host`, empty | `:131` | `advertise_host`, empty | **F3**: Macha falls back, `cluster.cpp:26-37`; Dannan does not, `node.cpp:30` |
| `network.port` | `port`, 7437, nonzero | `:133`, `config_base.cpp:78` | `port`, 7437, zero allowed | equal; the refusal stays Macha's |
| `network.inbound_capable` | `inbound_capable`, automatic | `:135` | `inbound`, `reach::Inbound::automatic` | equal; map the enum |
| `network.failure_domain` | `failure_domain`, empty | `:137` | `failure_domain`, empty | **F3**: Macha falls back to the host, `cluster.cpp:38` |
| `network.heartbeat_ms` | `heartbeat`, 5000 | `:139` | `heartbeat`, 5000 | equal |
| `network.telemetry_interval_ms` | `telemetry_interval`, 10000 | `:141` | `telemetry_interval`, 10000 | equal; floor of 250 in both |
| `network.dead_after_ms` | `dead_after`, 30000 | `:143` | `dead_after`, 30000 | equal |
| `network.connect_timeout_ms` | `connect_timeout`, 2500 | `:145` | `connect_timeout`, 2500 | equal; but see F16 |
| `network.max_frame_size` | `max_frame_size`, 256 KiB, 4K to 4M | `:147`, `config_base.cpp:153` | `max_frame`, 256 KiB | equal |
| `network.control_no_progress_deadline_ms` | `control_no_progress_deadline`, 30000 | `:149` | `control_no_progress`, 30000 | equal value, **different reach, F1**: pass 0 to the node and apply it in the facade |
| `network.data_no_progress_deadline_ms` | `data_no_progress_deadline`, 0 | `:152` | `data_no_progress`, 0 | same note |
| `network.control_stall_notice_ms` | `control_stall_notice`, 5000 | `:155` | `control_stall_notice`, 5000 | same note |
| `network.data_stall_notice_ms` | `data_stall_notice`, 120000 | `:158` | `data_stall_notice`, 120000 | same note |
| `network.upnp.enabled` | `upnp.enabled`, false | `:163` | `connectivity.upnp.enabled`, false | **F4**: and `gateway` must be set |
| `network.upnp.external_port` | `upnp.external_port`, 0 | `:165` | `connectivity.upnp.external_port`, 0 | equal |
| `network.upnp.discovery_timeout_ms` | `upnp.discovery_timeout`, 2000 | `:167` | `connectivity.upnp.discovery_timeout`, 2000 | equal |
| `network.upnp.lease_seconds` | `upnp.lease_seconds`, 0 | `:170` | `connectivity.upnp.lease_seconds`, 0 | equal |
| `network.external_ip.enabled` | `external_ip.enabled`, false | `:174` | `connectivity.external_ip.enabled`, false | equal |
| `network.external_ip.timeout_ms` | `external_ip.timeout`, 3000 | `:176` | `connectivity.external_ip.timeout`, 3000 | equal |
| `network.connectivity_check.enabled` | `connectivity_check.enabled`, false | `:181` | `connectivity.check.enabled`, false | equal |
| `network.connectivity_check.timeout_ms` | `connectivity_check.timeout`, 3000 | `:183` | `connectivity.check.timeout`, 3000 | equal |
| `bootstrap` | `bootstrap`, none; defaults the port from `network.port` | `:951` | `bootstrap` | parsed by Macha |
| not read from YAML | `inbound_reprobe_while_incapable`, 10 min | `config.hpp:634` | `reprobe_while_incapable`, 10 min | equal |
| not read from YAML | `inbound_reprobe_while_capable`, 1 h | `config.hpp:635` | `reprobe_while_capable`, 1 h | equal |
| not read from YAML | `dial_back_probe_min_interval`, 10 s | `config.hpp:638` | `dial_back_min_interval`, 10 s | equal |
| `maintenance.garbage_grace_ms` | `maintenance.garbage_grace`, 30 days | `:244` | `forget_after`, 30 days | equal; Macha's GC reads the same key, so it is copied |
| `runtime.memory_bytes` | `runtime.memory_bytes`, 0 | `:741` | `memory_limit_bytes`, 0, **F18** | equal; a name that means another thing |
| `runtime.retained_memory_bytes` | `runtime.retained_memory_bytes`, 768 MiB | `:743` | `memory_bytes`, 768 MiB, **F18** | equal |
| `runtime.control_memory_reserve_bytes` | 64 MiB | `:748` | `control_reserve_bytes`, 64 MiB | equal |
| `runtime.viewer_memory_reserve_bytes` | 192 MiB | `:751` | `foreground_reserve_bytes`, 192 MiB | equal |
| `runtime.loader_memory_reserve_bytes` | 64 MiB | `:754` | `loader_reserve_bytes`, 64 MiB | equal |
| `runtime.reassembly_memory_reserve_bytes` | 32 MiB | `:745` | `reassembly_reserve_bytes`, 32 MiB | equal |
| `runtime.glibc_arena_max` | 4 | `:739` | none | **no field**: stays Macha's, set before the node exists |
| `log_level` | `info` | `:846` | none; `log` is a sink | **no field**: stays Macha's, the sink's filter |
| `plugin_path` | none | `:844` | none; `SupervisorOptions::plugin_dir` | **no field**, by design: Dannan's supervisor is built apart from the node |
| `service_startup_no_progress_ms` | 120000 | `:860` | none; `StartupWatch::Options::no_progress`, 120000 | **no field**, by design; equal |
| `service_startup_timeout_ms` | 0 | `:857` | none; `StartupWatch::Options::ceiling`, 0 | **no field**, by design; equal |
| `storage.hosts_extents` | automatic | `:867` | none; the advert | **no field**: stays Macha's, the tristate and its resolution, `cluster.cpp:51-61` |
| `dht.metadata_write_copies` | 2 | `:195` | none; advert `macha/metadata-write-replicas` | **no field**: stays Macha's |
| `catalogue.api.enabled`, `.port`, `.advertised_endpoint` | off, 7438, empty | `:395-430` | none; hosted 1030 | **no field**: stays Macha's, feeds `telemetry_fields` |
| `streaming.*` playback budgets | `config.hpp:444-505` | `:668-732` | none; hosted 1031 to 1045 | **no field**: stays Macha's, feeds `telemetry_fields` |

Fields of `NodeConfig` with no Macha key, and what the facade sets:

| Field | Default | Macha's value |
|---|---|---|
| `version` | empty | `kServerVersion`, F11 |
| `gateway` | null | `std::make_shared<reach::UpnpGateway>()`, F4 |
| `external_ip`, `self_probe` | the built-ins when null, `node.cpp:570-571` | leave; `checkip_external_ip` is the lookup Macha logs as `source=aws` |
| `planes` | none | `data_storage`, `control_storage`, `cache`, `retention`, `metadata`, and the sixth of F5 |
| `telemetry_fields`, `telemetry_layout` | none | section 6 |
| `observation_interval`, `observation_max_bytes`, `observation_gauges` | 60 s, 64 MiB, none | equal to `service.cpp:560-565`; gauges from `Service`; F9 |
| `limits` | `net::ServerLimits` | leave. Checked against `net.cpp:110-115` and `net.hpp:239-246`: 2 fast, 2 control, 1 ordered, 8 data with 2 reserved, 64 jobs and 256 MiB ordered, 1, 16 and 64 MiB queues, 512 jobs: all equal |
| `log` | `ConsoleSink` | a sink over Macha's `Log::`, ignoring scope |
| `relay`, `path_class`, `punch_window`, `path_upgrade_every`, `relay_data_circuits`, `custody`, `certificate`, `ca_certificate`, `use_cluster_key` | off or null; `use_cluster_key` true | leave. A wire v1 cluster key cluster with level 0 idle. The two Pis need neither for the first cutover |
| `memory_warning`, `memory_fitted` | | leave; Macha's `validate` fits, so `memory_limit_bytes` stays 0 unless the fit moves, F18 |
| `dial_hook` | none | test fixtures only |
| `time`, `wall` | real | test fixtures only |

Everything else in `config.hpp` stays Macha's by group: `storage_backends`,
`storage_packing`, `ledger_cache`, `metadata_store`, `cache`, `maintenance.*` but the
grace, `filesystem`, `fuse`, `catalogue`, `ingest`, `torrent`, `books`, `streaming`,
`hydration`, `session`, `web`, `replication`, `write_copies`,
`retention_check_*`, `write_stall`, `extent_size`, `data_*`, `io_pressure_*`,
`read_ahead_extents`, `metadata_cache`, `metadata_materialization_cache_bytes`,
`ffmpeg_log_level`, `config_file`. The parser, `normalize_config`, the unknown-key
refusal and `configuration_warnings` are unchanged.

Findings in this section: **no default differs**; five keys have no `NodeConfig`
field by design or ruling; three are wrong to copy blindly (F1, F3, F4); one pair of
names invites a swap (F18).

## 4. State-file importer

All three files: big-endian integers, a string is a u32 length then its bytes,
written by `durable_replace_file`. `codec.cpp:5-39` and `net/codec.h:38-61` are the
same encoding.

### 4.1 `membership/known-nodes.bin`

Encoder `cluster/membership.cpp:111-161` (v4 only); reader `:35-109` (all four).
Macha writes v4 on every change and reads v1 to v4.

| Field | Width | MEM1 | MEM2 | MEM3 | MEM4 |
|---|---|---|---|---|---|
| magic `MACHMEM1`..`4` | 8 | yes | yes | yes | yes |
| count, at most 65536 | u32 | yes | yes | yes | yes |
| per node: id | 16 | yes | yes | yes | yes |
| host, bound 4096 | string | yes | yes | yes | yes |
| failure_domain, bound 4096 | string | yes | yes | yes | yes |
| port | u16 | yes | yes | yes | yes |
| seen_unix_ms | u64 | | yes | yes | yes |
| flags | u8 | | | yes | yes |
| last_seen_unix_ms (this node's wall clock) | u64 | | | | yes |
| reset count, at most 65536 | u32 | | yes | yes | yes |
| per reset: host | string | | yes | yes | yes |
| port | u16 | | yes | yes | yes |
| stale_node_id | 16 | | yes | yes | yes |
| epoch | u64 | | yes | yes | yes |
| reset_unix_ms | u64 | | yes | yes | yes |
| reset_by | 16 | | yes | yes | yes |
| reason | string | | yes | yes | yes |

Absent fields read as Macha reads them: `seen_unix_ms` 0, `flags` 3 (`types.hpp:50`),
`last_seen_unix_ms` the importer's wall clock (`membership.cpp:70`), no resets.
Nodes are sorted by id, resets by key. A node id of zero or this node's, an empty
host or a zero port is corrupt (`:71`), as is a duplicate. File at most 16 MiB.

`DNNMEM01`, encoder `membership/membership.cpp:89-121`, record `node_record.cpp:44-65`:

| `DNNMEM01` field | Width | From |
|---|---|---|
| magic | 8 | constant |
| count | u32 | count |
| id | 16 | id |
| host | string | host |
| port | u16 | port. **Order changes**: before `failure_domain` |
| failure_domain | string | failure_domain |
| seen_unix_ms | u64 | seen_unix_ms |
| incarnation | 16 | zero: unknown, a process that has gone |
| capabilities | u32 | `flags & 1` to `capability_inbound`; relay clear |
| path_class | u8 | 0, `public_path`, as a fresh node says it; replaced at first gossip |
| candidate count | u16 | 0 |
| advert count | u16 | 1 if `flags & 2`, else 0 |
| advert key, value | string, bytes | `macha/hosts-extents`, empty |
| last_heard_unix_ms | u64 | last_seen_unix_ms |
| reset count, resets | | byte for byte: the layouts are the same, `membership.cpp:108-116` |

Order by id and by key is the same (`IdentityReset::key()` equals
`identity_reset_key`, `identity_reset.cpp:27`, `types.cpp:51-60`).

What cannot be carried: nothing the file holds. `NodeInfo::capacity`, `used`,
`metadata_generation` and `metadata_write_replicas_required` are not in the file
either; Macha reloads them as zero and so does the import, until a peer is heard.
The import's `incarnation` of zero costs one `changed` per peer at first contact,
which the node would report anyway for a restart. The gain: Dannan's reader restores
`seen` from the last-heard wall time (`membership.cpp:61`), so `absent_since` survives
the restart where Macha's `stale` does not.

Older rosters: parity item 1 says "refused by name". Macha's reader accepts them, and
Macha is the plan, so the importer accepts all four. F17.

### 4.2 `connectivity/inbound.bin`

Encoder `cluster.cpp:434-445`, reader `:90-101`. Decoder `reach/inbound.cpp:44-48`,
encoder `:105-110`.

| Field | Width | `MACHINB1` | `DNNINB01` |
|---|---|---|---|
| magic | 8 | `MACHINB1` | `DNNINB01` |
| capable | u8 | nonzero is true | same |
| decided_unix_ms | u64 | | same |
| source (`probe:<hex id>`, `default`, `persisted`) | string, bound 4096 | | same |

The mapping is the magic alone. Nothing is lost: `last_probe_*` and the failure count
are not persisted by either. Both read the file only in `auto` mode and report
`persisted` afterwards. If it were not imported, the node would start as capable and
a truly incapable node would be dialled until two probes said otherwise, which is
what the file exists to prevent. Hence the import.

### 4.3 `telemetry/last-known.bin`

Encoder `cluster/telemetry.cpp:488-496`, `:140-225`; persisted `:728-753`, at most
1024 records, 16 MiB. `DNNTEL01`, `status/telemetry.cpp:65-75`, `:275-281`.

| Field | Width | Both |
|---|---|---|
| magic | 8 | `MACHTEL3`, `DNNTEL01` |
| record count | u32 | same |
| per record: length | u32 | same |
| per field: id | u16 | remapped below |
| size | u16 | same |
| value | size | byte for byte |

Macha's `field_traffic` (u32 window, u8 count, then u8 class, u64 in, u64 out, u32 and
u32 rates) is `Telemetry::set_traffic`'s layout exactly, as are all the widths of
`core_width`. `phase` is 0, 1, 2 in both. A record must keep a nonzero sequence.

| Macha id | Field | Dannan id |
|---|---|---|
| 1 to 8 | node_id, boot_id, sequence, observed_unix_ms, version, host, failure_domain, port | 1 to 8 |
| 14 | uptime_ms | core 9 |
| 15 | rss_bytes | core 10 |
| 16 | process_cpu_milli_percent | core 11 |
| 17 | load1_milli | core 12 |
| 19, 20 | peers_known, peers_active | core 13, 14 |
| 21, 22, 23 | rpc_connections_created, reused, canonical | core 15, 16, 17 |
| 24 | phase | core 18 |
| 26, 27 | cpu_cores, memory_total_bytes | core 19, 20 |
| 43 | node_name | core 21 |
| 44 | traffic | core 22 |
| the rest | | hosted, section 6 |
| over 44 | none at 0.92.3 | dropped and counted; Macha skips them too, `telemetry.cpp:418` |

What cannot be carried: nothing is lost. The file is observational, and its only reader
is Status's last-known fallback, `status_api.cpp:568`, `:583`. If it were not imported,
each node's capacity would read "unavailable" until a fresh ready sample came, and
offline nodes would show none at all. A bad file is ignored by both, so the tool can
fail without stopping a node.

### 4.4 Other state the transport or node owns

| File | Owner now | After | Survives |
|---|---|---|---|
| `node-id` | `load_or_create_node_id`, `storage/local_store.cpp:2159-2161` | `identity/node_identity.cpp:43-57`: same path, thirty-two hex and a newline | yes, untouched |
| `.macha.lock` | `StorageLock`, `local_store.cpp:413` | stays; `.dannan.lock` is taken beside it, `node_identity.h:23` | a new file; F9, F17 |
| `storage-layout` | `node_identity.cpp:17` | stays Macha's | yes; must be written before the node, F9 |
| `observation/observations.jsonl` | `service.cpp:563` | `node.cpp:463`: same path, cap and format | yes, appended; F9 |
| `identity/revocations`, custody | none | only with certificates: not this cutover | absent |

Macha holds no file for the UPnP mapping; it is removed on stop and re-made on start,
and `port_mapped_elsewhere` checks the mapping's client, not its description. The
others under the state path (`playback/transcode-rates.json`, `availability/`, the
ledgers, sessions, users, metadata) are the store's and Macha's, and do not move.

### 4.5 The tool

`dannan-import-macha <state>` is not built; parity item 1 specifies it. This
specification adds: read the node id, refuse a roster naming it; refuse while
`.macha.lock` or `.dannan.lock` is held (F17); take the advert name and the hosted
table from one place Macha owns (F17); keep each original as `*.macha`, which is
what rollback restores (F6).

## 5. Journal

Every log statement in the moving code, from a scan of `cluster/net.cpp`, `membership`,
`telemetry`, `public_connectivity`, `subsystem/*`, `cluster.cpp`, and the utilities
that move with them (`supervised`, `observation`, `diagnostics`, `contract/work`,
`config_base`). `membership.cpp` has none: the roster's lines are in `cluster.cpp`.
The text is Macha's expression with `std::string` and `substr` removed. "Macha" means
the facade writes it. Totals: 83 lines; 11 written, 2 merged, 11 reworded, 3 no
longer apply, 12 Macha's, **44 missing**.

| Macha | Level | Text | Dannan writes it? | Where |
|---|---|---|---|---|
| `cluster/net.cpp:87` | warn | `("peer sockets keep the system congestion control: ") + peer_congestion_control + " unavailable (" + strerror(errno) + ")"` | yes | `net/server.cpp:46` |
| `cluster/net.cpp:1632` | debug | `("peer writer: ") + error.what()` | merged | into `peer channel:`, `net/session.cpp:345` |
| `cluster/net.cpp:1711` | debug | `("peer channel: ") + error.what()` | yes | `net/session.cpp:345` |
| `cluster/net.cpp:2368` | info | `"node connection outbound peer=" + to_string(fresh->peer().id) + " lane=" + (transport_lane_name(lane)) + " endpoint=" + endpoint_key(endpoint)` | yes | `net/client.cpp:746` |
| `cluster/net.cpp:2532` | debug | `text` | reworded | `net/client.cpp:102`: peer is an id prefix, frame `read_ahead` |
| `cluster/net.cpp:2536` | debug | `("RPC stalled (") + frame_type_name(frame_type) + ") peer=" + (peer) + " message=" + message_type_name(type) + " age_ms=" + (elapsed_ms(call_started)) + " no_progress_...` | reworded | `net/client.cpp:96`: the same differences |
| `cluster/net.cpp:2607` | debug | `"peer " + endpoint_key(endpoint) + " liveness failure: " + reason` | reworded | `net/client.cpp:1274`: `host:port`, not `[host]:port` |
| `cluster/net.cpp:2693` | debug | `"dial request sent peer=" + to_string(peer) + " lane=" + transport_lane_name(lane)` | yes | `net/client.cpp:365` |
| `cluster/net.cpp:2703` | debug | `"dial request answered peer=" + to_string(peer) + " lane=" + transport_lane_name(lane)` | yes | `net/client.cpp:797` |
| `cluster/net.cpp:2774` | debug | `"lane maintenance peer=" + to_string(peer.id) + " lane=" + transport_lane_name(lane) + ": " + error.what()` | reworded | `net/client.cpp:715`: `node connection failed lane=... endpoint=...: why` |
| `cluster/net.cpp:2900` | debug | `"peer " + endpoint_key(probe.endpoint) + " lane=" + transport_lane_name(probe.lane) + " liveness failure: " + reason` | reworded | `net/client.cpp:1261` |
| `cluster/net.cpp:3173` | debug | `"shutdown: RpcClient::stop begin"` | reworded | `net/client.cpp:1328`: `shutdown: client stop begin` |
| `cluster/net.cpp:3206` | debug | `"shutdown: RpcClient::stop complete connections=" + (connections.size())` | reworded | `net/client.cpp:1340` |
| `cluster/net.cpp:3550` | debug | `("accepted peer writer: ") + error.what()` | merged | into `peer channel:`, `net/session.cpp:345` |
| `cluster/net.cpp:4073` | debug | `"shutdown: RpcServer::stop begin"` | reworded | `net/server.cpp:239` |
| `cluster/net.cpp:4087` | debug | `"shutdown: RPC accept thread joining"` | n/a | no accept thread on Asio |
| `cluster/net.cpp:4089` | debug | `"shutdown: RPC accept thread joined"` | n/a | no accept thread on Asio |
| `cluster/net.cpp:4102` | debug | `"shutdown: RPC sessions reaping count=" + (sessions.size())` | reworded | `net/server.cpp:240` |
| `cluster/net.cpp:4104` | debug | `"shutdown: RPC sessions reaped"` | reworded | `net/server.cpp:247` |
| `cluster/net.cpp:4137` | debug | `"shutdown: RPC queued requests dropped count=" + (dropped.size())` | **missing** | the scheduler drops silently |
| `cluster/net.cpp:4148` | debug | `"shutdown: RPC workers joining"` | **missing** |  |
| `cluster/net.cpp:4153` | debug | `"shutdown: RPC workers joined"` | **missing** |  |
| `cluster/net.cpp:4156` | debug | `"shutdown: RpcServer::stop complete"` | reworded | `net/server.cpp:249` |
| `cluster/net.cpp:4241` | debug | `"node connection probe accepted peer=" + to_string(session->peer.id) + " remote=" + session->remote_host` | yes | `net/server.cpp:180` |
| `cluster/net.cpp:4298` | info | `"node connection inbound peer=" + to_string(session->peer.id) + " lane=" + (transport_lane_name(session->channel->lane())) + " remote=" + session->remote_host + " adve...` | yes | `net/server.cpp:213` |
| `cluster/net.cpp:4386` | debug | `("RPC session: ") + error.what()` | yes | `net/server.cpp:176` |
| `cluster/net.cpp:4433` | debug | `("RPC handler: ") + error.what()` | yes | `net/server.cpp:89` |
| `cluster/telemetry.cpp:541` | warn | `"persisted telemetry ignored: " + (error.what())` | **missing** | silent catch, `status/telemetry.cpp:322` |
| `cluster/public_connectivity.cpp:439` | info | `"UPnP port mapping active gateway_external=" + status.upnp.external_address + ":" + (status.upnp.external_port) + " internal=" + status.upnp.lan_address + ":" + (confi...` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:444` | warn | `"UPnP gateway WAN address is private address=" + status.upnp.external_address + " probable_cgnat=true"` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:447` | warn | `status.upnp.error` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:452` | info | `"external IP discovered source=aws address=" + status.external_ip.address` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:454` | warn | `status.external_ip.error` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:469` | info | `"connectivity self probe reachable endpoint=" + status.advertised.host + ":" + (status.advertised.port) + " external_verification=unknown"` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:474` | warn | `"connectivity self probe failed endpoint=" + status.advertised.host + ":" + (status.advertised.port) + " external_reachability=unknown reason=\"" + status.self_probe_e...` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:495` | debug | `"UPnP owned mapping no longer matches; leaving external_port=" + ext` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:503` | debug | `"UPnP port mapping removed external_port=" + ext` | **missing** | `PublicConnectivity` has no logger |
| `cluster/public_connectivity.cpp:505` | debug | `"UPnP port mapping removal failed code=" + (result)` | **missing** | `PublicConnectivity` has no logger |
| `subsystem/subsystem_supervisor.cpp:99` | warn | `"subsystem plugin discovery failed dir=" + plugin_dir_.string() + " error=" + discovery_error.message()` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:112` | error | `"subsystem plugin '" + entry->name + "' failed to load: " + (::dlerror())` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:129` | debug | `"subsystem plugin candidate '" + entry->name + "' has no entry symbol '" + kSubsystemEntrySymbol + "', skipping as not a macha plugin: " + (symbol_error ? symbol_error...` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:140` | error | `"subsystem plugin '" + entry->name + "' entry function returned null"` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:149` | error | `"subsystem plugin '" + entry->name + "' build identity mismatch: plugin=" + (entry->plugin->build_identity) + " core=" + (kBuildIdentity) + "; refusing to load (partia...` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:216` | info | `"subsystem plugin '" + entry.name + "' loaded but its capability is not enabled on this node path=" + entry.origin()` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:225` | info | `"subsystem plugin '" + entry.name + "' loaded and running path=" + entry.origin()` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:251` | warn | `"subsystem '" + entry.name + "' stop() threw: " + e.what()` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:253` | warn | `"subsystem '" + entry.name + "' stop() threw an unknown exception"` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:263` | error | `"subsystem '" + entry.name + "' faulted while running: " + fault` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:267` | error | `"subsystem '" + entry.name + "' failed to start: " + fault` | **missing** | `supervise::Supervisor` has no logger |
| `subsystem/subsystem_supervisor.cpp:280` | error | `"subsystem '" + entry.name + "' disabled after " + (retry.failures_in_window()) + " failed attempts; needs an operator"` | **missing** | `supervise::Supervisor` has no logger |
| `cluster/cluster.cpp:105` | warn | `"inbound resolution ignored: " + (error.what())` | **missing** | silent catch, `reach/inbound.cpp:52` |
| `cluster/cluster.cpp:369` | warn | `"configuration: " + warning` | Macha | the facade keeps it |
| `cluster/cluster.cpp:372` | info | `("node inbound_capable=") + (resolution.inbound_capable ? "true" : "false") + " (" + (tristate_name(resolution.inbound_capable_mode)) + ", " + resolution.source + ") h...` | Macha | the facade, from `Diagnostics::inbound` |
| `cluster/cluster.cpp:388` | info | `"node " + to_string(identity_.id) + " listening on " + (server_.bound_port()) + " domain=" + members_.self().failure_domain + " state=recovering"` | Macha | the facade, from `Node::port()` |
| `cluster/cluster.cpp:469` | warn | `"inbound resolution not persisted: " + (error.what())` | **missing** | worse: the throw ends the loop, F12 |
| `cluster/cluster.cpp:481` | info | `("node inbound resolution changed inbound_capable=") + (inbound_capable ? "true" : "false") + " hosts_extents=" + (hosts ? "true" : "false") + " previous_inbound_capab...` | **missing** | `node.cpp:973` is silent, and nothing tells the facade |
| `cluster/cluster.cpp:550` | debug | `"dial-back probe not answered peer=" + to_string(peer->id) + " error=" + error` | **missing** | `node.cpp:1051` |
| `cluster/cluster.cpp:567` | debug | `"dial-back probe peer=" + to_string(peer->id) + " endpoint=" + self.host + ":" + (self.port) + " reachable=" + (reachable ? "true" : "false") + (error.empty() ? "" : "...` | **missing** | `node.cpp:1054` |
| `cluster/cluster.cpp:595` | debug | `"shutdown: node maintenance request_stop"` | n/a | no `request_stop`, F9 |
| `cluster/cluster.cpp:604` | debug | `"shutdown: cancelling outbound RPC calls"` | Macha | the facade can keep it |
| `cluster/cluster.cpp:610` | debug | `"shutdown: NodeRuntime::stop already stopped"` | Macha | the facade can keep it |
| `cluster/cluster.cpp:613` | debug | `"shutdown: NodeRuntime::stop begin"` | Macha | the facade can keep it |
| `cluster/cluster.cpp:617` | debug | `"shutdown: RpcServer::stop calling"` | Macha | the facade can keep it, as one step |
| `cluster/cluster.cpp:619` | debug | `"shutdown: RpcServer::stop returned"` | Macha | the facade can keep it, as one step |
| `cluster/cluster.cpp:620` | debug | `"shutdown: RpcClient::stop calling"` | Macha | the facade can keep it, as one step |
| `cluster/cluster.cpp:622` | debug | `"shutdown: RpcClient::stop returned"` | Macha | the facade can keep it, as one step |
| `cluster/cluster.cpp:629` | debug | `"shutdown: NodeRuntime::stop complete"` | Macha | the facade can keep it |
| `cluster/cluster.cpp:763` | debug | `"dial request received peer=" + to_string(peer.id) + " lane=" + transport_lane_name(lane)` | **missing** | `net/client.cpp:468` counts it only |
| `cluster/cluster.cpp:791` | debug | `"dial-back probe for peer=" + to_string(peer.id) + " endpoint=" + target.host + ":" + (target.port) + " reachable=" + (error.empty() ? "true" : "false") + (error.empty...` | **missing** | `reach/dial_back.cpp:58` |
| `cluster/cluster.cpp:907` | info | `"node advertised endpoint changed from=" + before.host + ":" + (before.port) + " to=" + status.advertised.host + ":" + (status.advertised.port) + " source=" + status.a...` | **missing** | `node.cpp:627` |
| `cluster/cluster.cpp:991` | debug | `"telemetry refresh skipped: " + (error.what())` | **missing** | silent catch, `node.cpp:965` |
| `cluster/cluster.cpp:1008` | info | `"node identity association reset scope=" + identity_reset_key(reset.host, reset.port) + " stale_node_id=" + (reset.stale_node_id == NodeId{} ? ("<any>") : to_string(r...` | **missing** | `node.cpp:1062` |
| `cluster/cluster.cpp:1031` | debug | `"identity reset propagation to " + peer.host + ": " + error.what()` | yes | `node.cpp:1106` |
| `cluster/cluster.cpp:1086` | info | `"membership: forgot " + (forgotten) + " node(s) not heard of for the absence horizon"` | **missing** | `node.cpp:900` |
| `cluster/cluster.cpp:1119` | debug | `"bootstrap: " + (error.what())` | **missing** | silent catch, `node.cpp:921` |
| `cluster/cluster.cpp:1133` | debug | `"peer " + node.host + ": " + error.what()` | **missing** | silent catch, `node.cpp:931` |
| `supervised.cpp:89` | error | `"thread '" + (name) + "' faulted (" + fault.code + "): " + fault.message + "; " + (outcome)` | **missing** | `supervise/supervised.cpp:50` records it, writes nothing; no logger |
| `observation.cpp:289` | debug | `"observation: cannot write " + path_.string()` | **missing** | no logger in `status/observation.cpp` |
| `observation.cpp:343` | debug | `"observation: gauge sampling failed: " + (error.what())` | **missing** | no logger in `status/observation.cpp` |
| `diagnostics.cpp:131` | debug | `thread_cpu_message("DIAG high thread CPU", name_, wall_ms, cpu_ms, pct, iterations_)` | **missing** | no `ThreadCpuReporter` in Dannan |
| `diagnostics.cpp:137` | debug | `thread_cpu_message("DIAG thread CPU recovered", name_, wall_ms, cpu_ms, pct, iterations_)` | **missing** | no `ThreadCpuReporter` in Dannan |
| `contract/work.cpp:55` | warn | `message` | yes | `sched/work.cpp:60` |
| `config_base.cpp:52` | warn | `"MEMORY OVER LIMIT: the configured parts total " + mib(total) + ", over runtime.memory_bytes " + mib(limit) + "; each shrinks by the same fraction: " + said + " (in-fl...` | Macha | keep Macha's validate; `node.cpp:408` says `foreground` otherwise |

The missing lines, by what an operator loses:

- A thread's fault (`supervised.cpp:89`, error) and all twelve subsystem lines,
  among them `disabled after N failed attempts; needs an operator`.
- All nine connectivity lines: mapping active, CGNAT warning, self-probe failure.
- Inbound changes (`cluster.cpp:481`) and the endpoint change (`:907`), both info.
- `node identity association reset` (`:1008`), info, and `membership: forgot`
  (`:1086`), info.
- The bootstrap and peer exchange failures (`:1119`, `:1133`), the first thing read
  when two nodes will not meet. Parity item 2 lists the transport's dial failures and
  not these.
- `DIAG high thread CPU`, which the diagnostics memory relies on.

F13 says how to close them. The rewording is within parity's scope, but a watch
that greps `RPC stalled (read-ahead)` or a `[host]:port` will miss.

## 6. Hosted telemetry layout

A proposal; the layout is Macha's to own, and Dannan's code holds no Macha field.
Fields Dannan's core does not carry, numbered from `field::first_hosted`, in Macha's
id order. Written by `NodeConfig::telemetry_fields`; `telemetry_layout` lists them in
this order. As in Macha, a zero or empty value is not written (`Telemetry::set_*`)
and an absent one reads as "did not say".

| Hosted id | Macha id | Macha field | Width | Encoding |
|---|---|---|---|---|
| 1024 | 9 | `storage_capacity` | 8 | `set_u64` |
| 1025 | 10 | `storage_used` | 8 | `set_u64` |
| 1026 | 11 | `cache_capacity` | 8 | `set_u64` |
| 1027 | 12 | `cache_used` | 8 | `set_u64` |
| 1028 | 13 | `metadata_generation` | 8 | `set_u64` |
| 1029 | 18 | `storage_backends_online` | 4 | `set_u32` |
| 1030 | 25 | `api_endpoint` | var | `set_text`, no terminator |
| 1031 | 28 | `playback_startup_timeout_ms` | 4 | `set_u32` |
| 1032 | 29 | `playback_segment_timeout_ms` | 4 | `set_u32` |
| 1033 | 30 | `playback_pipeline_idle_ms` | 4 | `set_u32` |
| 1034 | 31 | `playback_session_idle_ms` | 4 | `set_u32` |
| 1035 | 32 | `playback_max_sessions_per_account` | 4 | `set_u32` |
| 1036 | 33 | `playback_max_sessions` | 4 | `set_u32` |
| 1037 | 34 | `playback_transcode_entitlement_idle_ms` | 4 | `set_u32` |
| 1038 | 35 | `cache_hits` | 8 | `set_u64` |
| 1039 | 36 | `cache_misses` | 8 | `set_u64` |
| 1040 | 37 | `cache_evictions` | 8 | `set_u64` |
| 1041 | 38 | `playback_max_transcodes_per_account` | 4 | `set_u32` |
| 1042 | 39 | `playback_startup_no_progress_ms` | 4 | `set_u32` |
| 1043 | 40 | `playback_start_wait_max_ms` | 4 | `set_u32` |
| 1044 | 41 | `playback_start_failed_retention_ms` | 4 | `set_u32` |
| 1045 | 42 | `playback_transcode_rates` | var | below; `set_bytes` only when not empty |

`playback_transcode_rates`, unchanged from `telemetry.cpp:185-200`, `:374-391`:

| Field | Width | Bound |
|---|---|---|
| count | u16 | at most 256; more is cut on write, refused on read |
| per rate: kind | string (u32 length) | 16 on read |
| codec | string (u32 length) | 64 on read |
| bit_depth | u32 | video only, as rendered |
| height_class | u32 | video only, as rendered |
| rate_milli | u32 | |
| observations | u32 | |
| concurrent | u32 | |

At most 2 + 256 x (4 + 16 + 4 + 64 + 20) = 27,650 bytes, inside the field's u16 size.
Dannan carries it unread, so the facade's decoder holds the bounds and `finish()` and
must drop one record, not the set, when one fails: `Telemetry::u32` throws on the wrong
width where Macha's `decode` refused the whole set.

Not hosted, because the core has them: every field of section 4.3's mapping.

`peer_viewers_active(fresh_for)` is derived as `cluster.cpp:717-729` does, in the
facade: for each view of `Context::telemetry().views(fresh_for)` that is `fresh` and
not this node's, walk `view.telemetry.traffic()` (core 22); true if any entry whose
`frame_class` is foreground (2) or read-ahead (3) has a nonzero `in_bytes_per_s` or
`out_bytes_per_s`. The wire values are equal (`frame_type.hpp:11-17`,
`sched/work.h:28-34`), the node counts both directions in one `TransportTraffic`
(`node.cpp:477`, `:495`), and rates are over the sender's last interval as before.
No gap. One small difference: on a node relaying circuits, `relay_data` is counted by
the class it carries; the two Pis relay nothing.

## 7. Message numbers

### 7.1 Macha's types that become Dannan's

Not re-registered. Dannan's rules from `net/routes.cpp:30-70`, checked against
`allowed_on_lane` (`net.cpp:150-168`), `class_allowed` (`:480-488`),
`is_notification_message` (`:407-410`) and `fast_control_request` (`:3805-3808`): all
agree. Macha's handlers for these move (`cluster.cpp:747-836`) and `Routes::bind`
refuses them, `node.cpp:130`.

| Type | Macha now | Dannan core | Dannan's lanes | Frames | Notification | Fast control | Payload |
|---|---|---|---|---|---|---|---|
| `ping` | 1 | 1 | both | control_only |  | yes | unchanged: empty, answered `ok` |
| `members` | 2 | 2 | control | control_only |  | yes | request empty; reply differs, below |
| `session_retire` | 12 | 3 | both | control_only |  |  | Dannan's own; Macha never sends it by name |
| `promote_read_ahead` | 14 | 4 | both | control_only |  |  | transport control; Macha calls `promote` |
| `promote_foreground` | 15 | 5 | both | control_only |  |  | transport control; Macha calls `promote` |
| `cancel_transfer` | 16 | 6 | both | control_only |  |  | transport control; Macha calls `cancel` |
| `telemetry` | 24 | 7 | control | any | yes |  | **differs**: `DNNTEL01` set, core field ids, hosted from 1024 |
| `identity_resets` | 25 | 8 | control | control_only |  |  | **differs**: magic `DNNRST01` for `MACHIDR1`; body unchanged |
| `dial_request` | 43 | 9 | control | control_only | yes |  | unchanged: u8 lane |
| `dial_back_probe` | 44 | 10 | control | control_only |  |  | unchanged bytes (string host, u16 port); named `dial_back` |
| `ok` | 100 | 100 | both | any |  |  | unchanged: empty |
| `error` | 101 | 127 | both | any |  |  | unchanged bytes (string message, u16 status, string code); number moves from 101 |
| `members_reply` | 102 | 101 | control | control_only |  |  | **differs**: `NodeRecord` for `NodeInfo`; no capacity, used, generation, replicas or flags, those are adverts |
| `telemetry_reply` | 109 | 102 | control | any |  |  | **differs**: `DNNTEL01` set |
| `identity_resets_reply` | 110 | 103 | control | control_only |  |  | **differs**: `DNNRST01` |
| `dial_back_probe_reply` | 120 | 104 | control | control_only |  |  | unchanged bytes (u8 reachable, string error); named `dial_back_reply` |

Dannan's `ok` and `error` stand in for the transport's own refusal too: where Macha
answers an overfull queue with an empty `error` (`net.cpp:3936`, `:4379`), Dannan
answers 503 `overloaded` (`net/scheduler.cpp:17`). Both read as try-later.

### 7.2 Macha's types, re-registered

Proposed number: 512 plus the current one, so holes stay holes and a number is read
at sight. 37, 38, 48 and 124 are not used and stay unregistered: 549, 550, 560, 636
(F20). Each registers with `Routes::add` before `Node::start`, under Macha's name, and
the handler binds later as today. Payloads unchanged.

The columns come from Macha's own rules: lanes from `allowed_on_lane`; frames from
`class_allowed`, the five bulk types `not_control`, `is_priority_data_message` and
`is_node_record_message` `any`, the rest `control_only`; notification from
`is_notification_message`; ordered from `metadata_mutation_request`
(`net.cpp:3810-3819`); no re-registered type is fast control today. Dannan's `Lanes`,
`Frames` and `Registration` express all of them. `lane_for`'s four DATA types are
Dannan's rule by registration (`net/client.h:241`), and no type needs its own
`no_progress` or `stall_notice`: `stall_notice_for` and `no_progress_deadline_for`
pick by the same four types (`cluster.cpp:632-644`).

| Type | Now | Proposed | Lanes | Frames | Notification | Fast control | Ordered | Note |
|---|---|---|---|---|---|---|---|---|
| `have_object` | 3 | 515 | control | any |  |  |  |  |
| `get_object` | 4 | 516 | data | not_control |  |  |  |  |
| `put_object` | 5 | 517 | data | not_control |  |  |  |  |
| `get_metadata` | 6 | 518 | control | any |  |  |  |  |
| `cas_metadata` | 7 | 519 | control | any |  |  |  | no sender or handler outside `net.cpp`: dead, kept for the number |
| `seed_metadata` | 8 | 520 | control | any |  |  |  | no sender or handler outside `net.cpp`: dead, kept for the number |
| `metadata_notice` | 9 | 521 | control | control_only | yes | yes (proposed) |  | read inline on the session thread today, `net.cpp:1664`, `:4317`; F10 |
| `get_committed_metadata` | 10 | 522 | control | control_only |  |  |  |  |
| `checkpoint_metadata` | 11 | 523 | control | any |  |  |  | no sender or handler outside `net.cpp`: dead, kept for the number |
| `delete_object` | 13 | 525 | control | any |  |  |  |  |
| `commit_metadata` | 17 | 529 | control | any |  |  |  | no sender or handler outside `net.cpp`: dead, kept for the number |
| `cas_metadata_delta` | 18 | 530 | control | any |  |  |  | no sender or handler outside `net.cpp`: dead, kept for the number |
| `get_control_object` | 19 | 531 | control | any |  |  |  |  |
| `put_control_object` | 20 | 532 | control | any |  |  |  |  |
| `get_metadata_identity` | 21 | 533 | control | any |  |  |  |  |
| `put_object_deferred` | 22 | 534 | data | not_control |  |  |  |  |
| `object_durability_barrier` | 23 | 535 | data | not_control |  |  |  |  |
| `get_metadata_history_entry` | 26 | 538 | control | any |  |  |  |  |
| `has_metadata_history_entry` | 27 | 539 | control | any |  |  |  |  |
| `put_metadata_history_entry` | 28 | 540 | control | any |  |  | yes |  |
| `get_metadata_heads` | 29 | 541 | control | any |  |  |  |  |
| `put_metadata_commit` | 30 | 542 | control | any |  |  | yes |  |
| `accept_metadata_commit` | 31 | 543 | control | any |  |  | yes |  |
| `retain_objects` | 32 | 544 | control | any |  |  |  |  |
| `get_ingest_jobs` | 33 | 545 | control | any |  |  |  |  |
| `ingest_job_action` | 34 | 546 | control | any |  |  |  |  |
| `get_torrent_jobs` | 35 | 547 | control | any |  |  |  |  |
| `torrent_job_action` | 36 | 548 | control | any |  |  |  |  |
| `session_sync` | 39 | 551 | control | control_only | yes |  |  | also called as a request, answered by its reply type |
| `have_objects` | 40 | 552 | control | any |  |  |  |  |
| `get_metadata_history_record` | 41 | 553 | control | any |  |  |  |  |
| `user_sync` | 42 | 554 | control | control_only | yes |  |  | also called as a request, answered by its reply type |
| `have_control_objects` | 45 | 557 | control | control_only |  |  |  |  |
| `torrent_intent` | 46 | 558 | control | any |  |  |  |  |
| `have_valid_objects` | 47 | 559 | control | any |  |  |  |  |
| `trie_diff` | 49 | 561 | control | any |  |  |  |  |
| `get_unmatched_hints` | 50 | 562 | control | any |  |  |  |  |
| `submit_ingest_job` | 51 | 563 | control | any |  |  |  |  |
| `bool_reply` | 103 | 615 | control | any |  |  |  |  |
| `object_reply` | 104 | 616 | data | not_control |  |  |  |  |
| `metadata_reply` | 105 | 617 | control | any |  |  |  |  |
| `cas_reply` | 106 | 618 | control | any |  |  |  | no sender or handler outside `net.cpp`: dead, kept for the number |
| `control_object_reply` | 107 | 619 | control | any |  |  |  |  |
| `metadata_identity_reply` | 108 | 620 | control | any |  |  |  |  |
| `metadata_history_entry_reply` | 111 | 623 | control | any |  |  |  |  |
| `metadata_heads_reply` | 112 | 624 | control | any |  |  |  |  |
| `ingest_jobs_reply` | 113 | 625 | control | any |  |  |  |  |
| `ingest_job_action_reply` | 114 | 626 | control | any |  |  |  |  |
| `torrent_jobs_reply` | 115 | 627 | control | any |  |  |  |  |
| `torrent_job_action_reply` | 116 | 628 | control | any |  |  |  |  |
| `session_sync_reply` | 117 | 629 | control | control_only |  |  |  |  |
| `have_objects_reply` | 118 | 630 | control | any |  |  |  |  |
| `user_sync_reply` | 119 | 631 | control | control_only |  |  |  |  |
| `have_control_objects_reply` | 121 | 633 | control | control_only |  |  |  |  |
| `torrent_intent_reply` | 122 | 634 | control | any |  |  |  |  |
| `have_valid_objects_reply` | 123 | 635 | control | any |  |  |  |  |
| `trie_diff_reply` | 125 | 637 | control | any |  |  |  |  |
| `unmatched_hints_reply` | 126 | 638 | control | any |  |  |  |  |
| `ingest_submit_reply` | 127 | 639 | control | any |  |  |  |  |

## 8. Findings

Ranked by what they would break at the cutover. "Facade" means no Dannan change;
"Dannan" means Dannan changes, Macha being the truth.

**Would break the cluster or the store**

- **F1. The no-progress deadline reaches every call.** Macha applies it, and the stall
  notice, only in the synchronous `call` (`net.cpp:2513-2546`, `cluster.cpp:646-676`);
  `call_async` has neither (`cluster.cpp:678-704`). Dannan arms a watchdog for every
  call on a lane (`net/client.cpp:583-609`). So CONTROL-lane async calls Macha leaves
  to their callers would be cancelled at 30 s of quiet: `have_valid_objects`
  (`distributed_store.cpp:1945`), which reads, decrypts and hashes up to 16 objects
  per request at speculative class on a Pi; `have_objects` (`:1871`),
  `put_control_object` (`:2185`); `put_metadata_history_entry` (`metadata_manager.cpp:693`),
  `get_committed_metadata` (`:1409`). Under load that is failed repairs and publications
  where Macha waits. Fix, facade: pass zero for all four `NodeConfig` deadlines and
  notices, and run Macha's own `wait_for_reply` loop over `CallHandle::idle_for`.
  That also keeps Macha's exact journal lines.
- **F2, ruled by Tom:** every call specifies its deadline, with no default and no blanket facade constant; Macha adapts in the refactor. The recommendation below did not stand.
- **F2. `Routes::call` requires a wall deadline; Macha has none.** The rule "no call waits
  without a deadline" (`context.h:47-51`) against `data_no_progress_deadline` 0, "off for
  data, whose transfers have their own logic" (`config.hpp:685-688`). A facade constant
  long enough never to fire reproduces Macha; a short one cancels long transfers. Each
  call also builds a strand and a timer (`node.cpp:843-844`), on the DATA path,
  unmeasured. Needs Tom: a named constant in the facade, or a form with no wall deadline.
  Recommendation: the constant, 24 h, and `dannan_bench` over this path before the Pis.
- **F3. An empty advertised host makes a node invisible.** Macha falls back to the listen
  address or `gethostname`, and the failure domain to the host (`cluster.cpp:26-38`).
  Dannan takes both as given (`node.cpp:30-32`), and a peer drops a record with no host
  (`membership/membership.cpp:210`). Without the fallback, a node with no
  `network.advertise` is never rostered, and an empty domain merges nodes in placement's
  domain groups, moving owners. Fix, facade: run `self_info`'s logic before `NodeConfig`.
- **F4. UPnP is off unless the facade builds a gateway.** `NodeConfig::gateway` defaults
  to null, which reads as `support_not_built` (`reach/connectivity.cpp:106`, `:162`).
  Whether the two Pis map a port is each node's `macha.yaml`, not in this tree. A node
  reached through its mapping would lose inbound, and dial-back would then call it
  incapable. Fix, facade: set
  `std::make_shared<reach::UpnpGateway>()`. The real IGD run still waits on Tom.
- **F5. `Readiness` is not `RecoveryProgress`.** Parity item 4 reads them as equal. Macha's
  `complete` is a separate flag (`local_state.hpp:58-66`), set at `service.cpp:522` after
  `LocalServices` is built, later than the five `mark`s (`local_state.cpp:114-174`).
  Dannan's is "every plane ready" (`status/readiness.cpp:68-75`), so a node says `ready`
  and stamps `ready_at_unix_ms` early. Peers then treat its fresh zero capacity as
  authoritative (`status_api.cpp:203`). Also, `RecoveryProgress::fail` keeps the first
  failure (`local_state.hpp:49-54`), `Readiness::fail` the last (`status/readiness.cpp:56-57`).
  Fix: the facade declares a sixth plane, `local_services`, marked at `service.cpp:522`,
  and Status reads the five by name. Dannan: keep the first failure.
- **F6. Rollback is not clean.** The evaluation says "nothing on disk changed", but the
  importer replaces `known-nodes.bin` in place, and 0.92.x refuses `DNNMEM01` as corrupt
  (`membership.cpp:52-53`), so the old binary does not construct. Inbound and telemetry
  only warn (`cluster.cpp:103-107`, `telemetry.cpp:539-543`). Fix, runbook: rollback
  restores the three `*.macha` files, and loses only what was learned since.

**Would change behaviour during the soak**

- **F7. An advert change is a topology event.** Macha's `NodeEvent::topology` fires for
  roster, endpoint, flags and replicas changes, not for capacity or generation
  (`cluster.cpp:867-871`, `:890-893`): the comment there says identical gossip must not
  restart maintenance's GC quiet window. Dannan's `topology` includes every advert
  (`node.cpp:46`), and `used` changes with every write. Dannan's `membership` is close,
  but excludes the adverts `hosts_extents` and the replicas figure now live in, and
  those must move placement "as for a join or leave" (`cluster.cpp:474-475`). Also, the
  `metadata` event and `remote_metadata_generation` were computed on the way past in
  three places (`cluster.cpp:225-232`, `:264-274`, `:860-863`). Fix, facade: consume
  neither Dannan kind; derive Macha's from `Nodes::watch`, diffing the adverts: topology
  on `joined`, `left`, a place change, `macha/hosts-extents` or the replicas advert;
  metadata on a higher generation. Records of inactive nodes no longer count; `settle`
  absorbs it.
- **F8. The node's activity clocks would count work Macha does not.** `Node::handle` notes
  every inbound request (`node.cpp:680`). Macha notes only object bytes it served
  (`storage_server.cpp:202`, `:236`) and the API's work (`service.cpp:84`), and
  maintenance's quiet rule reads viewer presence. A peer's metadata call at foreground
  frame would hold off GC, rebalance and scrub here, and object bytes would count twice.
  Fix, facade: Macha keeps its own `ActivityClocks` in `NodeResources` for this cutover
  and does not read `Context::activity()`. Revisit at the second.
- **F9. Lifecycle and construction order.** `request_stop` has no home (`service.cpp:603`,
  `:623`): add `Node::request_stop`, Dannan. The node owns the ledger it charges frames
  to, with no injection (`node.h:290`), but `NodeResources` builds Macha's first
  (`node_resources.cpp:19`), and `DataResourceArbiter` is built beside it. So the root
  holds `.macha.lock`, then runs the layout check, then builds the node, then the rest
  taking `node.memory()`. The order matters: `load_v18_node_id` refuses any entry but
  `.macha.lock` when the marker is absent (`node_identity.cpp:31-36`), so a fresh
  node whose `.dannan.lock` is made first refuses to start. The recorder moves into the
  node (`node.cpp:462`): remove `service.cpp:560-568`, or two writers share one file. It
  now starts at `Node::start`, after whatever precedes it in `Service`, and stops inside
  `Node::stop`, so the `shutdown` event at `service.cpp:645` must precede `node_.stop()`.
- **F10. `metadata_notice` loses its inline path.** Macha reads it on the session thread
  ahead of every queue (`net.cpp:1664-1668`, `:4317-4321`), which is why metadata
  convergence does not wait behind control work. A hosted notification goes through the
  scheduler (`net/session.cpp:149`), behind two control workers. Fix, facade: register it
  `notification` and `fast_control`; two fast workers, so a notice can pass another, which
  the epoch logic tolerates. Its handler must not block.
- **F11. `NodeConfig::version` must be `kServerVersion`.** It feeds the record's version
  (`node.cpp:950`) and the observation log (`:464`); Macha's log says `kBuildIdentity`,
  `service.cpp:564`. `runs_at_least` reads the record to choose `trie_diff` and
  `get_unmatched_hints`. Status's `version` must match, so the log's value changes. Name
  it in the commit.
- **F12. A failed inbound write ends the connectivity loop.** Macha catches and warns
  (`cluster.cpp:466-470`). `InboundResolver::persist` throws through `observe_probe`, and
  `node.cpp:1052-1058` catches only `DecodeError`, so a full disk restarts the loop under
  backoff, re-running UPnP. Fix, Dannan.

**Would fail the journal and Status gates**

- **F13. 44 journal lines are missing** (section 5). The facade cannot write most: nothing
  tells it of a dial-back, an endpoint change or a thread's fault. Fix, Dannan: a logger
  on `PublicConnectivity`, `Supervisor`, `SupervisedThreads` and `ObservationRecorder`;
  lines in `Node`'s three loops, `DialBackResponder` and `handle_dial_request`, at Macha's
  levels and words. A thread CPU reporter, or Tom's word that `DIAG` goes.
- **F14. Names in Status cannot be byte-identical without a decision.** `threads[]` has
  `cluster-telemetry`, `cluster-maintenance`, `cluster-connectivity` where the node says
  `node-telemetry`, `node-membership`, `node-connectivity` (`cluster.cpp:393-399`,
  `node.cpp:580-582`). `net-peer-reader`, `net-peer-writer`, `net-health`, `net-rpc-writer`,
  `net-rpc-accept`, `net-rpc-session` and the four `net-rpc-*-worker` rows have no
  counterpart: there are no such threads, and the scheduler's workers are not supervised.
  Rename the three, Dannan or facade; rule on the ten. `frame_timings` must use Macha's
  `read-ahead` (`net.cpp:638`), not `sched::frame_type_name`'s `read_ahead`.
  `message_timings` needs `dial_back` shown as `dial_back_probe` and its reply likewise.
  New core types (relay, channel, revocations) appear only if used.
- **F15, as of 0.92.4:** Macha now reports the transport's real created, reused and canonical counts, and counts a dial only after the identity check. The text below describes `00861dd`. Dannan must match the new behaviour; `node.cpp:969` still has the old form.
- **F15. `rpc_transport.canonical_connections` has no source: the one Status gap.** Macha
  counts usable routes, both directions (`net.cpp:3004-3009`). `ClientStats` has none.
  `connections_created` differs too: Macha counts after the handshake (`:2304`), Dannan's
  `dials` at the attempt (`net/client.cpp:322`, `:524`), so use `dials - dial_failures`.
  Fix, Dannan: a `canonical` count in `stats()`. Masked in the golden, but still rendered.

**Smaller**

- **F16. Shorter budgets on the node's own calls.** Exchanges wait `connect_timeout * 2`,
  5 s, and dial-back `* 3`, 7.5 s (`node.cpp:883`, `:1050`); Macha gives both 30 s of
  no progress. A loaded Pi could fail an exchange and undercount `peers_active`. Liveness
  is unaffected, since health pings stand apart. Measure in the soak.
- **F17. The importer.** It must refuse on `.macha.lock` as well as `.dannan.lock`, or a
  running 0.92.x is not seen. Accept `MACHMEM1` to `4`, against parity's "refused by name":
  Macha reads them, and the layout costs nothing. The advert name and hosted table are
  Macha's vocabulary; parity allows them in the tool, but they should be read from one
  table with the facade.
- **F18. `memory_bytes` means two things.** Macha's `runtime.memory_bytes` is
  `NodeConfig::memory_limit_bytes`, and Macha's `retained_memory_bytes` is
  `NodeConfig::memory_bytes`. Keep Macha's `validate` as the one fit
  (`config_base.cpp:26-57`), pass its results, and leave the limit at 0: the warning keeps
  the word `viewer` and nothing fits twice.
- **F19. `Nodes::snapshot` is not one reading.** `context.h:94` says it is; `node.cpp:69-73`
  takes the lock twice, where Macha holds it once (`membership.cpp:317-334`). A node can
  appear active and not known. Harmless to Status, one line to fix. Dannan.
- **F20. Retired numbers are not refused.** `context.h:147-148` says so; `RouteTable::add`
  checks only taken (`net/routes.cpp:76-81`). Register 549, 550, 560 and 636 as nothing,
  or add the list. Dannan.
- **F21. The replicas advert has no reader.** `NodeInfo::metadata_write_replicas_required`
  is set (`cluster.cpp:44`) and read only to detect a change (`:870`); the evaluation's
  "must set and check" overstates it. Carry it, for the topology event, and check nothing.
- **F22. Plugins rebuild.** Macha's `subsystem_abi.hpp` and Dannan's `PluginEntry` differ,
  and `SubsystemContext` reaches the plugin as `std::any`. FUSE and torrent are built
  together with the core, so this is deploy work, covered by the plugin gate.

### Macha defects noticed

For the Macha session; none fixed here. None blocks the cutover.

| Where | What |
|---|---|
| `cluster/cluster.cpp:949-950` | `rpc_connections_created` and `reused` are passed as literal 0, so `nodes[].runtime.rpc_connections_created` and `reused` are always 0, and `canonical` is `peers_active - 1`, not a count of connections. `rpc_stats()` has the real figures. Dannan reproduces it, `node.cpp:954` |
| `cluster/cluster.cpp:225-229`, `:854`, `:877` | `remote_metadata_generation_` is loaded and later stored, where the server's path uses a CAS (`:267-271`): a racing higher value can be overwritten by a lower one until the next heartbeat. The facade should not copy it |
| `types.hpp:61-63` | Says a mismatched replica floor fails closed. No code reads a peer's value but the change check at `cluster.cpp:870` |
| `cluster/net.cpp:638`, `api/status_api.cpp:359` | The same frame class is `read-ahead` in `frame_timings` and `read_ahead` in `traffic.classes`, both visible to clients |
| `cluster/net.hpp:35-46`, `:117` | `seed_metadata`, `cas_metadata`, `cas_metadata_delta`, `checkpoint_metadata`, `commit_metadata` and `cas_reply` have no sender or handler outside `net.cpp` |
| `cluster/net.cpp:2304` | `connections_created_` is counted before the expected-id check, so a dial that reached the wrong node counts as created |
