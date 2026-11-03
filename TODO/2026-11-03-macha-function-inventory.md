# What Macha does that Dannan and Dagda must do, and Macha must still do

Status: inventory, 2026-11-03, from a read of `../macha` at `6c7ec64`
(0.91.2, the baseline). One row per behaviour of the code the design
places in Dannan or Dagda, with where it lives in Macha, which library it
goes to, and what proves it. Since the reversal of 2026-11-03 (evening;
`ACTIVE.md`) each row is read twice: it is the specification of a
`dannan_tests` or `dagda_tests` case written here, from the design and
`macha/docs/`, before the piece is built (a build step that cannot name
the row's case does not ship); and its Macha case is the gate of the port
step that replaces the piece in Macha (a port step that cannot name the
proof for a behaviour it touches does not happen). After a build step the
row gains the Dannan or Dagda case; after a port step it keeps the Macha
case as the port's gate. The "Where" column is Macha's location, read for
what the code does and never copied.

"Test file" without a case name means the suite file owns the invariant
and the case is still to be named. Gates named `VALIDATION` are in
`macha/VALIDATION.md`; `crash matrix` is `macha/docs/durability.md`.

## Transport and lanes (`cluster/net.*`) - to Dannan, rewritten on Asio at seam 0

Every row here is the gate for seam 0 before it is the gate for any move:
the proof column is filled for this table first.

Filled 2026-11-03 from a read of each case body at `6c7ec64`, not from
case names. A cell says **proven** when the named cases assert the whole
behaviour, **partial** when they assert some of it (the rest is named),
**none** when no case asserts it. Unprefixed case names are in
`tests/test_rpc_cluster.cpp`, group `rpc_cluster`; `invariants/` cases
are in `test_invariants.cpp`. "To write" names the case still owed, from
the behaviour's specification, before seam 0 moves the row.

| Behaviour | Where | Proof |
|---|---|---|
| Two lanes per peer, CONTROL and DATA, plus a probe lane that authenticates and closes; a lane is dialled on demand and reused; concurrent cold callers share one dial | `net.hpp:146`, `RpcClient::connection` | **proven**: `test_rpc_v15_bidirectional_and_deduplication` (DATA opened lazily, reused both ways, cross-dial winner, alias dedup, retirement drains), `test_rpc_concurrent_cold_data_calls_share_one_dial` (24 callers, one dial), `test_rpc_v15_persistence_and_multiplexing` (reuse, no reconnect after a slow call), `test_mutual_bootstrap_prunes_cross_dial`, `test_a_call_whose_dial_is_retired_by_a_simultaneous_connect_waits_for_the_peer`, `test_rpc_a_dial_in_flight_is_waited_for_no_longer_than_a_dial`; probe lane: `test_inbound_incapable_node_is_reached_only_over_its_own_sessions` (`probe_dial` creates no canonical connection); lane enforced by the receiver: `invariants/test_authenticated_receiver_enforces_transport_lane` |
| Frame priority order on a transport queue; control never queues behind data; slow control does not abort a data transfer | `frame_type_priority`, `RpcServer` queues | **proven**: `test_rpc_v15_frame_priority_and_variable_length` (the order; a foreground put overtakes a 32 MiB speculative one), `test_rpc_slow_control_does_not_abort_data`, `test_rpc_held_executors_do_not_delay_other_classes` |
| BBR, keepalive and no Nagle on every peer socket; one warning where the kernel has no BBR | `peer_socket_options`, `net.hpp:200` | **proven** except the single warning: `test_peer_sockets_use_delay_based_congestion_control` (BBR, Linux), `test_peer_sockets_disable_nagle_and_keep_alive` (added: `TCP_NODELAY`, `SO_KEEPALIVE` and a 60 s first probe on the sockets the transport dialled, both lanes, every platform). The accepted side is asserted only through the same `peer_socket_options` |
| Outbound queue limits per connection (256 messages, 128 MiB) with a reserve for small messages so no number of large replies crowds a ping out | `outbound_refusal`, `net.hpp:161` | **proven** on the caller's side: `test_large_replies_never_crowd_out_a_small_message` (the function), `test_a_ping_is_admitted_behind_a_full_queue_of_large_messages` (added: a connection to a peer that stops reading refuses large messages and still admits a ping). **Finding**: on the server's side the same limit drops replies; see "Findings" below |
| Fragment reassembly bounded in messages and bytes beyond the per-message limit; a message's memory leases follow it through executor handoffs | `MessageAssembler`, `RpcMessage::retained_memory` | **proven**: `test_rpc_reassembly_has_count_byte_and_message_bounds`, `test_rpc_reassembly_is_process_memory_charged_until_consumed`, `test_rpc_request_payload_is_charged_until_handler_completion` (the lease held through the server's handoff until the handler returns); `foundations/test_retained_memory_ledger_never_starves_rpc_reassembly`, `foundations/test_retained_memory_reassembly_reserve_is_bounded_not_absolute` |
| Pending-reply limit per connection (512); a route that refuses says why | `max_pending_rpc_requests`, `call_existing(why)` | **proven**: `storage_v18/test_a_control_graph_larger_than_the_connection_budget_still_publishes`, `test_a_connection_holds_no_more_than_its_pending_reply_budget` (added: 512 outstanding on one connection, the 513th refused with "peer pending reply limit reached" in the error, the budget usable again). That every admitted call is answered is not asserted: see "Findings" |
| Stall notice and no-progress deadline per message type, configurable per lane; a call with no progress fails transiently for the caller to retry | `stall_notice_for`, `no_progress_deadline_for`, config `*_stall_notice`, `*_no_progress_deadline` | **partial**: `test_rpc_call_fails_after_no_progress_deadline` (the transient error names the message and "cancelled for retry"; a slow call inside the deadline is untouched; zero means none), `test_rpc_v15_persistence_and_multiplexing` and `test_rpc_slow_control_does_not_abort_data` (the interval is a notice, not a deadline); the config keys parse in `runtime_dependencies/test_yaml_config`. Added: `test_no_progress_deadline_follows_the_lane_configured` (a node configured with one lane bounded at 300 ms and the other unbounded, both ways round). The stall notice is a DEBUG line by specification and stays unasserted |
| An in-flight call can be cancelled, aborted or promoted to a higher frame class, on both sides (`promote_read_ahead`, `promote_foreground`, `cancel_transfer`) | `AsyncRpc`, `RpcServer::promote_queued`, `cancel_queued` | **partial**: cancel: `test_rpc_v15_frame_priority_and_variable_length` (a cancelled 32 MiB transfer is not requeued and the connection survives), `test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` (a queued job is removed and never runs); abort: the same case (a running job finishes and its reply is discarded); ownership: `test_async_rpc_move_ownership`. **Promotion is unproven**: the earlier citation of `test_media_playback.cpp` was wrong, because no test calls `promote_read_ahead` or `promote_foreground` (`hydration_catalogue` exercises the replica selector's `promoted`, a different thing). Added: `test_a_promoted_call_overtakes_on_both_sides` (a read-ahead queued behind held DATA workers runs once promoted, as foreground; a speculative call queued behind a 32 MiB transfer leaves first once promoted). Now **proven** |
| Frame size negotiated at the handshake, bounded by config | `SecureChannel::client_handshake`, `max_frame_size` | **partial**: `test_rpc_v15_frame_priority_and_variable_length` sends a payload that is not a multiple of the frame size with both sides at 4096; `runtime_dependencies/test_yaml_config` parses the key. Added: `test_peers_with_different_frame_limits_agree_on_the_smaller` (both ways round; nothing larger is sent), `test_a_negotiated_frame_above_the_clients_limit_is_refused`. Now **proven** |
| Authenticated encrypted channel: AEAD per fragment with counters, a session id, shutdown and close under one mutex | `SecureChannel` | **partial**: AEAD primitives in `foundations/test_codec_and_crypto` (seal, open, a flipped bit refused); a wrong lane rejected on the header in `invariants/test_authenticated_receiver_enforces_transport_lane`; a bad MAC refused in `test_rpc_v7_handshake_is_rejected` only incidentally. Added, over a socketpair relay the test sits in: `test_a_tampered_fragment_ends_the_session` (body and header), `test_a_replayed_fragment_ends_the_session`, `test_a_peer_with_another_cluster_key_fails_the_handshake`, `test_both_ends_of_a_session_share_its_id_and_no_other_session_does`. Now **proven**; "shutdown and close under one mutex" is a thread-safety annotation, checked by the compiler |
| Protocol version in the handshake; any other version fails it, never negotiates down | `net.cpp:78`, `:886`, `:939` | **partial**: `test_rpc_v7_handshake_is_rejected` (an older hello is closed without a reply, server side). Added: `test_a_hello_from_a_newer_protocol_is_refused` (24 closed without a reply, 23 answered), `test_a_client_refuses_an_ack_of_another_version` (22 and 24 refused, 23 accepted). Now **proven**. The byte-for-byte proof for seam 0 is the two-process test against the 0.91.2 binary |
| Server request classes with byte-bounded queues per class, a fast-control allow-list, metadata workers with per-peer FIFO, rejection counters, per-type and per-frame timings in Status | `RpcServer`, `RpcServerExecutionLimits`, `RpcServerWorkStats` | **proven** for the server: `test_rpc_metadata_mutations_use_bounded_isolated_executor` (per-class byte limits, rejection counters, per-type and per-frame timings), `test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers`, `test_rpc_metadata_control_mutations_go_first`, `test_rpc_held_executors_do_not_delay_other_classes` (fast control answers with every worker held), `test_rpc_job_traffic_travels_as_its_own_class`. Their rendering in Status: the Status golden (to commit, Next 1) |
| Executor cancellation on disconnect and shutdown; a pending synchronous call fails at once so workers can leave an RPC wait before being joined | `cancel_outbound_calls`, `RpcClient::stop` | **proven**: `test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` (server side), `test_service_shutdown_cancels_pending_outbound_rpc_before_join` (a blocked synchronous call fails within 2 s of `stop`; no new route afterwards) |
| `broadcast`, `broadcast_best_effort` (skips when the route table is busy) and `broadcast_notify` (waits for the table, never dials, never waits on a peer) | `RpcClient`, `net.hpp:622` | **partial**: `test_best_effort_telemetry_notifications_reach_both_route_directions` proves `broadcast_best_effort` delivers on both route directions, at speculative and control. Added: `test_broadcast_notify_never_dials_and_never_waits_on_a_silent_peer`. Still unasserted: `broadcast` itself, and `broadcast_best_effort` skipping while the route table is busy, which no public interface can arrange (the table lock is never held across anything a test supplies); needs a test seam, Tom's call |
| An inbound-incapable node is reached only over sessions it opened; a peer asks for a lane with `dial_request` and waits a bounded time for the reverse dial; the node keeps both lanes dialled to every maintained peer | `RpcClient::await_reverse_dial`, `set_maintained_peers`, `dial_request` | **proven**: `test_inbound_incapable_node_is_reached_only_over_its_own_sessions` (refused at once without a dial, control both ways over the site's session, DATA by `dial_request`, a severed DATA lane redialled unasked). The bound on the reverse-dial wait is not asserted on its own: covered in "to write" for the next row |
| `inbound_capable: auto` resolved by a peer's dial-back probe, persisted, re-probed by cadence (10 min while incapable, 1 h while capable); one probe per asking peer per 10 s | `NodeRuntime::connectivity_loop`, `probe_dial`, `dial_back_probe_min_interval` | **partial**: `test_inbound_auto_resolves_from_dial_back_and_survives_restart` (resolved from two failed dial-backs, persisted across restart, reversed by a successful one, gossiped to the hub). Added: `test_dial_back_probes_are_rate_limited_per_asking_peer`, `test_reprobe_cadence_defaults_follow_the_resolution`, `test_a_reverse_dial_that_never_comes_fails_within_its_bound`. Now **proven** |
| A cluster with no inbound-capable node is refused with a reason | `refuse_impossible_cluster` | **proven**: `test_inbound_auto_resolves_from_dial_back_and_survives_restart`, its last block (a founding node with `inbound_capable: false` and no bootstrap throws, naming "founding node") |
| Session reaping; a cap on sessions not yet authenticated | `RpcServer::reap_sessions`, `pre_auth_sessions_` | **partial**: `invariants/test_rpc_pre_auth_admission_is_bounded` proves 32 idle sockets cost no thread each, **on Linux only**; it is skipped on macOS, so the laptop gate never runs it. Added: `test_unauthenticated_connections_are_bounded_on_every_platform` (beyond the bound an idle connection is closed at once, the rest held, the bound frees as they go). Reaping is still unasserted: nothing a test can observe counts sessions; needs a test seam, Tom's call |
| Peer health: heartbeat pings, failure backoff per peer, smoothed CONTROL round trip from pings only | `RpcClient::health_loop`, `observe_result` | **partial**: `test_a_ping_answered_while_the_health_pass_ran_late_keeps_its_route`, `test_a_hung_health_probe_is_retried_inside_the_liveness_budget` (heartbeats and liveness); `test_rpc_v15_bidirectional_and_deduplication` (a failed dial's backoff does not mask an inbound route); `test_rpc_v15_frame_priority_and_variable_length` (a smoothed latency exists after calls). Added: `test_round_trip_is_smoothed_from_pings_not_from_slow_replies`, `test_dial_backoff_is_per_peer`. Now **proven** |
| Cluster traffic in and out by frame class, one instance shared by client and server | `TransportTraffic`, `traffic_totals` | **proven**: `test_traffic_counts_both_directions_by_frame_class_in_one_instance` (added: a fragment's bytes with their 56 bytes of overhead, every class equal across two nodes, a server session counted in the client's instance); its Status rendering is the Status golden |
| A reply to an admitted request is never dropped for capacity (0.91.3, `d61eb02`): refused by the outbound limits or the memory lease, it is replaced by a 503 `transport_busy` error reply ("reply not sent: why") with no lease, allowed past the cap up to `max_pending_rpc_requests` extra messages; past that the connection closes, which answers every pending call; a non-reply refusal still throws | both `queue_message` implementations, `RpcServer::execute` | **proven** in Macha at `d61eb02`: `test_a_connection_holds_no_more_than_its_pending_reply_budget` (all 512 ready within 20 s, each a `bool_reply` or a `transport_busy` refusal, at least 224 real replies); `docs/ownership.md`, the RPC bullet |

### Findings (2026-11-03)

- **Replies beyond 256 on one connection are dropped unanswered.** The
  client admits up to `max_pending_rpc_requests` (512) calls awaiting
  replies per connection; the server's session queues replies under the
  same `outbound_refusal` (256 messages). When more than 256 complete in a
  burst, `RpcServer::execute` (`net.cpp:4384-4405`) catches the refusal,
  tries an error reply, has that refused too, and drops it: the caller
  hears nothing and waits for its no-progress deadline, or for ever with
  none. Reproduced on the laptop at `6c7ec64`: 512 `have_object` calls held
  at a gate and released, 356 answered, 156 never. Not fixed; Tom's call
  (ACTIVE, Waiting on Tom). `test_a_connection_holds_no_more_than_its_pending_reply_budget`
  deliberately does not assert that every call is answered until it is.
- **Two behaviours need a test seam** to be proven at all: best-effort
  broadcast skipping while the route table is busy, and finished sessions
  being reaped. Both are internal state no public interface exposes in
  Macha. Settled for Dannan by design (`ACTIVE.md`, Decisions): its
  server's work stats carry a session count and its client's route table
  has a test-only hook, so both rows are proven here.
- The dropped-replies finding below is **fixed in Macha at 0.91.3**
  (`d61eb02`, the Macha session, 2026-11-03) and is now a row of the
  transport table.
- `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`
  is flaky on the laptop at the baseline: it failed 1 run in 5 on the
  untouched 0.91.2 test source, and in both full-gate runs on the branch.

## Membership (`cluster/membership.*`, `types.hpp`) - to Dannan

| Behaviour | Where | Proof |
|---|---|---|
| Gossiped roster; direct liveness within `dead_after` counts, gossip does not; `active()` and `all()` | `Membership::observe`, `directly_reachable` | `test_rpc_cluster.cpp` |
| A node unheard of for the forgetting horizon (`garbage_grace_ms`, never under twice `dead_after`) is dropped; gossip that old does not teach it back; its own connection does | `forget_unseen`, config | `test_rpc_cluster.cpp`, `test_absent_nodes.cpp`; `docs/cluster.md` |
| The roster is persisted under the state path in sequence order, encoded under the lock and written after it; an older roster is skipped | `encode_known_locked`, `write_known`, `persist_m_` | `test_rpc_cluster.cpp` |
| Identity association resets: an endpoint re-keyed to a new node id, applied, propagated, persisted in the head, surfaced in Status and the management API | `apply_identity_reset`, `propagate_identity_reset`, `IdentityAssociationReset` | `test_rpc_cluster.cpp`, `test_web_api.cpp`; `docs/management.md` |
| Flags gossiped: `inbound_capable`, `hosts_extents`; unknown ids report true | `set_flags`, `inbound_capable(id)` | `test_rpc_cluster.cpp` |
| `all_known_reachable` excludes pairs that both refuse inbound | `membership.hpp:81` | `test_rpc_cluster.cpp` |
| `metadata_write_replicas_required` advertised; a mismatch fails closed so a weaker node cannot mint a certificate | `NodeInfo`, `MetadataServer::accept_commit` | `rpc_cluster/test_two_node_mutual_bootstrap_metadata_write_floor` (known flaky; ACTIVE section 1 there) |
| Failure domain per node from config, carried in the roster | `NodeInfo::failure_domain` | `test_foundations.cpp` (placement) |
| Bootstrap endpoints as seeds, not masters | `Config::bootstrap`, `NodeRuntime::exchange` | `test_rpc_cluster.cpp` |
| Storage, cache and metadata-generation figures advertised with the node's record and republished on change | `advertise_*`, `publish_self` | the Status golden |

## Placement (`cluster/placement.*`) - to Dannan

| Behaviour | Where | Proof |
|---|---|---|
| Plain rendezvous hashing for capacity-independent placement | `rendezvous_nodes` | `test_foundations.cpp`, `test_models.cpp` |
| Capacity-aware placement over the 2^32 shard space: preferred owners then deterministic fallbacks, distinct failure domains preferred, no free-space feedback loop | `capacity_placement_nodes` | `test_foundations.cpp`; VALIDATION "Placement and the write floor" |
| R=1 logical capacity aggregates eligible nodes rather than the smallest | `placement_logical_capacity` | VALIDATION "Admission and capacity" |
| A node without `hosts_extents` is never a candidate | the candidate filter | `test_rpc_cluster.cpp` |
| Commit replicas ordered local first, then by measured latency, unmeasured last | `order_commit_replicas` | `test_foundations.cpp` |

## Public connectivity (`cluster/public_connectivity.*`) - to Dannan

| Behaviour | Where | Proof |
|---|---|---|
| UPnP mapping discovered, created, verified, owned, removed on exit; the status codes `igd_not_connected`, `port_mapped_elsewhere` and the rest | `PublicConnectivity`, `UpnpStatus` | `test_rpc_cluster.cpp` (status shape); real IGD on gbni-1 |
| External IP lookup; advertised endpoint and its source; the self-probe | `ExternalIpStatus`, `PublicConnectivityStatus` | the Status golden |
| Status never waits on a probe (two mutexes, ordered) | `work_mutex_` before `mutex_` | `test_rpc_cluster.cpp` |

## Telemetry and status (`cluster/telemetry.*`, `cluster/activity_clocks.*`) - to Dannan, fields grandfathered

| Behaviour | Where | Proof |
|---|---|---|
| Telemetry gossiped every `telemetry_interval`, sooner on a readiness change or peer observation, at most once a second | `telemetry_loop`, `signal_telemetry_refresh` | `test_rpc_cluster.cpp` |
| The record: phase, version, host, failure domain, storage and cache figures, cache activity, metadata generation, uptime, RSS, CPU, load, peers, RPC stats, traffic by class, playback budgets, transcode rates | `NodeTelemetry` | the Status golden; `GET /api/v1/status/nodes` |
| Peer viewers active in a fresh sample pace repair as local viewers do | `peer_viewers_active` | `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers` |
| Last-known node status kept in the head for Status's fallback | `PersistedNodeStatus` in the snapshot | `test_storage_metadata.cpp` |
| Activity per work class: presence without bytes (an API request) and bytes per frame; `idle_for`; 24 h when never present | `ActivityClocks` | `test_work_class.cpp` |
| Node events as monotonic counts per kind, consumers diff them, nothing calls out of a producer's thread | `NodeEvents` | `test_contract.cpp` |

## Work classes, waits and budgets (`contract/work.*`, `retained_memory.hpp`, `contract/walk.hpp`) - to Dannan

| Behaviour | Where | Proof |
|---|---|---|
| One definition of class from frame type; `frame_for(class)`; control has no DATA frame | `frame_type.hpp` | `test_work_class.cpp`. **Dannan** (step 1): `FrameType.*` and `WorkClass.*` in `dannan/test/sched_work_test.cpp` (wire values fixed, queue priority, decode refuses other bytes, one definition, `frame_for` round trip); foreground replaces viewer |
| Declared waits per operation and the runtime wait guard; control may not wait on the DATA device or the network; tests switch the guard to throw | `Waits`, `WaitGuard` | `test_contract.cpp`, `test_work_class.cpp`. **Dannan** (step 1): `Waits.*`, `WorkContext.*`, `WaitGuard.*` in `sched_work_test.cpp`; the guard is an injected object, not a static |
| Three memory reserves plus the reassembly reserve inside one retained total; a memory limit shrinks the configured parts pro rata with a `MEMORY OVER LIMIT` warning | `RuntimeConfig`, `RetainedMemoryLedger` | `test_contract.cpp`; the memory gauges (0.90.69/70). **Dannan** (step 1): `RetainedMemory.*` and `MemoryLimit.*` in `sched_memory_test.cpp` (each reserve by class, the waiter gates, reassembly's reserve and its exemption, shedding, restore, cancellation, deadline, stop); owners are open (Dannan below 256, hosted systems above) |
| Budgets, cursors and pages for paged long work | `contract/walk.hpp` | `test_predicates.cpp`, `test_namespace_entries.cpp`. **Dannan** (step 1): `Budget.*`, `Page.*` in `sched_walk_retry_test.cpp`; time is passed in |
| Retry policy with backoff, a failure budget and a disabled state | `retry_policy.hpp` | `test_subsystem_supervisor.cpp`. **Dannan** (step 1): `Retry.*` in `sched_walk_retry_test.cpp` (backoff to the ceiling, the window, the backstop, success keeps the window, reset); the disabled state is the supervisor's, step 4c |
| Published immutable snapshots behind a swap | `contract/published.hpp` | `test_contract.cpp`. **Dannan** (step 1): `Published.*` in `contract_test.cpp` |
| A per-account bound beside every node-wide bound on viewer resources (law 2, second clause) | per-session limits, `max_concurrent_password_checks` | `test_session.cpp`, `users/test_accounts_converge_across_nodes` (`try_later`). **Dannan**: owed at step 5, with the Budget noun |

## Supervision (`subsystem/*`, `startup_progress.hpp`) - to Dannan

| Behaviour | Where | Proof |
|---|---|---|
| Plugins discovered from the plugin directory; a missing directory loads none; the build identity is checked before any call in; a mismatch is refused | `SubsystemSupervisor::discover_plugins`, `subsystem_abi.hpp` | `test_subsystem_supervisor.cpp` with `test_plugin_mismatched_abi`, `test_plugin_no_entry_symbol` |
| A factory returning null means "not run here", reported `unavailable`, not retried; a throw is a failure under backoff and disable | `SubsystemPluginEntry::create` | `test_plugin_declining`, `test_plugin_faulting` |
| A fault reported after start destroys and rebuilds the instance; first reason wins; calls after stopping are ignored | `Subsystem::attach_fault_sink` | `test_plugin_faulting_after_start` |
| Stop runs the subsystem's stop and it may write | `test_plugin_writes_on_stop` | the same |
| Builtin subsystems supervised like plugins | `add_builtin` | `test_fuse_subsystem.cpp` |
| Statuses in `GET /api/v1/status` `threads` (name, running, restarting, faults, last fault, its code and time) | `SubsystemStatus`, `status_api.cpp` | CLIENT-CONTRACTS 0.63.0; the Status golden |
| A startup making no progress for `service_startup_no_progress` exits for the supervisor; the absolute timeout is off by default | `startup_progress.hpp`, `Service` | `test_service_root.cpp` |
| A construction that may block (FUSE waits for the first namespace) is cancelled by the lifecycle thread's stop token | `SubsystemContext::startup_stop` | `test_fuse_subsystem.cpp` |

## Identity, time, state files - to Dannan

| Behaviour | Where | Proof |
|---|---|---|
| A durable node id created on first start under the state path; a fresh durability epoch per process, so peers tell a restart from a reconnect | `NodeIdentity` | `test_storage_v18.cpp`, `test_rpc_cluster.cpp` |
| The cluster key file; `SecureChannel` keyed from it | `ClusterKeys`, `key_file` | `test_rpc_cluster.cpp` |
| The state path is locked before any state file is read | `StorageLock` | `test_service_root.cpp` |
| Injected time for every interval decision; the steady clock for logs only | `TimeSource`, `SteppedTime` | `tests/TESTING.md`, every timed case. **Dannan** (step 1): `SteadyTimeSource.*`, `SteppedTime.*` in `contract_test.cpp`; the stepped clock is `dannan/test/fixtures/stepped_time.h` |
| Observation windows sampled to `observation/observations.jsonl` | `observation.*` | `test_observation.cpp` |
| Durable file replacement (write, sync, rename) | `durable_file.*` | `test_storage_v18.cpp`. **Dannan** (step 1): `DurableFile.*` in `identity_durable_file_test.cpp` (new and replaced files, owner-only, failure leaves the old state and no debris) |
| Log levels, scoped prefixes, the glibc arena cap | `log.*`, `process_allocator.*` | `test_foundations.cpp`. **Dannan** (step 1): `LogLevel.*`, `ScopedLogger.*`, `ConsoleSink.*` in `status_log_test.cpp`; a node's sink is injected, there is no process logger. The glibc arena cap is a setting of the hosted executable's process, not a library behaviour: Macha keeps it |

## Configuration (`config.hpp`, `config.cpp`) - split at seam 1

| Behaviour | Where | Proof |
|---|---|---|
| Every key in `macha.yaml.example` parsed to the same normalized value; unknown keys refused at start; `configuration_warnings` for legal but notable settings | `load_yaml_config`, `normalize_config` | `macha-tests-runtime` config cases; the config golden |
| `inbound_capable` and `hosts_extents` tristates with `automatic` resolved from evidence; a setting that contradicts the network refused | `Tristate`, `apply_inbound_resolution` | `test_rpc_cluster.cpp`; discipline 3 |
| Sizes, endpoints and durations parsed as documented | `parse_size`, `parse_endpoint` | `test_foundations.cpp` |
| Live reload of the reloadable limits | `NodeServices::reconfigure`, `reconfigure_local` | `test_node_services.cpp` |

## The store (`storage/`, `ledger/`, `metadata/`, `cluster/distributed_store.*`) - to the store

Coarser rows: the store moves whole at seam 3 and is reshaped only at
seams 6 to 8. Each row's proof is the suite that owns it today and the
VALIDATION and crash-matrix gates, which do not change.

| Behaviour | Where | Proof |
|---|---|---|
| Objects by SHA-256 of plaintext; encryption and packing below the id; loose and packed representation; pack index rebuilt by scan; torn tail truncated; compaction preserves every live object; tombstones | `storage/local_store.*`, `sealed_journal.*` | `test_storage_v18.cpp`; VALIDATION "Physical representation" |
| Two stores with separate quotas; DATA exhaustion cannot block a CONTROL write; `reserve_free` headroom; several local backends with deterministic fallback | `StoragePool`, `LocalStore` | VALIDATION "Admission and capacity", "Class separation" |
| `put` returns the holders and tiers; `put_deferred` and the durability barrier batch `syncfs`; tickets fenced by epoch and backend incarnation; strict writes | `DistributedStore`, `DurabilityDomain` | `test_storage_metadata.cpp`; crash matrix rows 3 and 4 |
| `has(id)` is the ground truth every check falls back to; cheap presence, not a decrypt | `ObjectStore::has`, `PresenceIndex` | `storage_v18/test_has_is_a_cheap_presence_check_not_a_decrypt` |
| Held view and identity: the held trie's root per backend, the union across disks, removals count, completeness while seeding | `held_view`, `TrieUnion`, `HeldLedger` | `test_object_ledger.cpp` |
| Retention claims as per-origin dots; release only of dots the observed clock dominates; claims from an unseen branch survive a partition | `RetentionStore`, `ClaimStore` | `test_ledger_gates.cpp`, `test_absent_nodes.cpp`, `users/test_a_node_that_was_down_learns_a_deletion_not_a_resurrection` |
| GC: two sweeps, the grace period, a copy with any local claim untouchable, deletion lazily durable, the three destructive gates | `storage/retention.*`, `contract/gates.*`, `maintenance.*` | `test_ledger_gates.cpp`, `test_catalogue_maintenance.cpp`; crash matrix "GC unlink lost" |
| The trie, the diff over the wire from one pinned snapshot, the merge join, the survey by diff and `unavailable`, repair's push and pull lists | `ledger/object_trie.*`, `trie_diff.*`, `availability_service.*`, `distributed_store.cpp` | `test_object_ledger.cpp`, `test_availability.cpp`; `2026-10-09-replication-by-diff.md` in Macha |
| Repair paced, never stopped; the push list from the diff; no probe where a diff gave the answer | `repair_step` | `rpc_cluster/test_repair_is_paced_not_stopped_while_a_peer_serves_viewers`, `test_availability.cpp` |
| Local state recovery plane by plane; the fresh-genesis refusal of unversioned or mismatched state and backends | `LocalState`, `RecoveryProgress` | VALIDATION "Refusing unversioned storage"; `test_storage_v18.cpp` |
| Control objects fetched from any peer at speculative priority without DATA credit | `ControlObjectFetch` | `test_storage_metadata.cpp` |
| Persistent block cache, never counted for durability | `PersistentBlockCache` | `test_storage_metadata.cpp` |
| DATA credit arbiter: in-flight bytes, the viewer reserve, I/O pressure from service time, background admission held under pressure, the no-progress deadline on a wedged arbiter | `DataResourceArbiter`, `io_pressure.hpp` | `test_io_pressure.cpp`, `test_work_class.cpp` |
| Commits immutable and content-identified, full or delta, multi-parent; acceptance at the write floor with a certificate; heads as a set; branches reconcile without a common ancestor; conflicts as durable records; retries idempotent by dot; history truncated out of the hot path; the namespace tree re-rooting | `MetadataManager`, `MetadataReplica`, `metadata_merge.cpp`, `namespace_tree.*` | `test_metadata_merge.cpp`, `test_namespace_tree.cpp`, `test_namespace_migration.cpp`, `rpc_cluster/test_metadata_history_checkpoint_concurrent_proposers_converge`; `docs/metadata.md` |
| Snapshot memory bounded: shared payload backing, streamed hashes, deltas in place | `SharedBytes`, `MetadataRecord` | VALIDATION "Memory" |
| Metadata serving: commits accepted and imported, heads announced, generation advertised and settled | `MetadataServer` | `test_rpc_cluster.cpp` |
| The torrent request record: field-wise merge, claim epochs, the tombstone, the duplicate rule, hard bounds at decode | `torrent/torrent_request.*` | `test_models.cpp`, the torrent cases |
| The claim walk | `service/claim_walk.*` | `test_claim_walk.cpp` |

## Operational surfaces - unchanged, by Macha

| Behaviour | Where | Proof |
|---|---|---|
| `GET /api/v1/status`, `/status/nodes`, `/status/diagnostics`, `/health` answered inline by the reactor | `api/status_api.cpp`, `service.cpp:87` | the Status golden; `test_http_server.cpp` |
| Route classes: health, status, session and users are control; everything else viewer; served bytes keep a viewer present | `service.cpp:73`, `HttpServer::set_activity` | `test_web_api.cpp`, `test_http_server.cpp` |
| The tools: `macha-metadata-dump`, `macha-metadata-repair`, `macha-namespace-migrate`, `macha-users`, `macha-recover` | `tools/` | they build and run on a fixture state directory |
| The install tree, rpaths, the private libdir, the plugin directory, the systemd unit, the config example | `CMakeLists.txt`, `systemd/` | the install check (seam gate 6) |
| Deploy and rollback on the cluster | `build/deploy.sh`, `/root/pre-<version>/` | a deploy to fi-1 after the seam |
| Every client contract | `TODO/CLIENT-CONTRACTS.md` | `test_web_api.cpp`; announced to Core before any change, and none is planned |
