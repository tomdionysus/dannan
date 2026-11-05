# Macha test mapping for the first cutover

Status: written 2026-11-05 for the Macha session to confirm. Nothing is ported or built. Macha at `00861dd` (the read-only copy at `build/scratch/macha-00861dd`), Dannan at `feature/macha-parity` `4742cb4`. It answers section 6 item 5 of `../macha/TODO/2026-11-04-dannan-dagda-port-evaluation.md`, under Tom's ruling: a Macha case whose subject becomes Dannan's may be retired against a named `dannan_tests` case, case by case; everything Macha-visible is rewritten over the new fixtures.

## How to read it

Fates: **retire** (the named Dannan body was opened and asserts the same behaviour), **rewrite** (stays in Macha over new fixtures, with the nouns it needs), **keep** (Macha's own code, unchanged), **gap** (Dannan has no case proving it; see the Gaps table), **obsolete** (only what Tom ruled away, and the ruling is named). A case covering several things has a fate for each part, so a case can count under more than one fate.

The rule was applied strictly: where a Dannan case proves part of a Macha body, the rest is a gap, even when the code plainly does it, so most gaps are cheap. A Dannan case at the queue or the scheduler stands for Macha's over a socket only where the behaviour is that layer's, and the row says so where it matters.

"Believed yes" in the Gaps table means the code was not read closely enough to say. Every case name in the rows was checked by script against `dannan/test`, and every Macha name against the extract. The fates of the 55 store and metadata cases in `test_rpc_cluster.cpp` were sorted by the fixture they use, not judged line by line, since those cases are out of scope.

## Counts by fate

Cases in scope. A case with two fates counts under each; "wholly retired" is those with no other fate.

| File | Cases | retire | wholly retired | rewrite | keep | gap | obsolete |
|---|---:|---:|---:|---:|---:|---:|---:|
| `test_rpc_cluster.cpp` | 59 | 37 | 20 | 11 | 4 | 27 | 3 |
| `test_foundations.cpp` | 30 | 23 | 14 | 2 | 3 | 15 | 2 |
| `test_contract.cpp` | 17 | 10 | 7 | 2 | 1 | 7 | 0 |
| `test_work_class.cpp` | 4 | 3 | 3 | 0 | 1 | 0 | 0 |
| `test_observation.cpp` | 17 | 12 | 3 | 2 | 0 | 12 | 0 |
| `test_subsystem_supervisor.cpp` | 4 | 3 | 1 | 1 | 0 | 2 | 0 |
| `test_absent_nodes.cpp` | 2 | 1 | 0 | 0 | 0 | 2 | 0 |
| `test_models.cpp` | 1 | 1 | 1 | 0 | 0 | 0 | 0 |
| `test_invariants.cpp` | 9 | 2 | 1 | 7 | 1 | 1 | 0 |
| `test_root_parts.cpp` | 2 | 0 | 0 | 1 | 0 | 1 | 0 |
| `test_service_root.cpp` | 4 | 0 | 0 | 4 | 0 | 0 | 0 |
| `test_runtime_dependencies.cpp` | 1 | 0 | 0 | 0 | 1 | 0 | 0 |
| `test_baselines.cpp` | 1 | 0 | 0 | 1 | 0 | 0 | 0 |
| `test_node_services.cpp` | 1 | 0 | 0 | 1 | 0 | 0 | 0 |
| **Total** | **152** | **92** | **50** | **32** | **11** | **67** | **5** |

Outside those: 55 store and metadata cases of `test_rpc_cluster.cpp`, 35 rewrite and 20 keep (listed below), and 17 cases of `test_foundations.cpp` that test Macha's own config, spool, arbiter, torrent and FUSE code, all keep. The rest of `test_rpc_cluster.cpp`'s 114 are in the table above.

48 gaps follow, of which 4 are behaviour Dannan's code does not have (G43, G44, G45, G48).

## Seams

**Verdict: no Macha-visible behaviour needs a send or admit seam in Dannan. Do not build them.** `NodeConfig::dial_hook` is also not needed by any Macha case at this cutover.

Macha's `FaultyLinks` (`tests/test_rpc_cluster.cpp:5217`) implements `RpcLinks` and is used by 10 cases. Seven test the transport and do not count, by the ruling. Three are Macha-visible and use one thing, `stall(peer, type)`:

| Case | Seam used | Macha-visible | What replaces it |
|---|---|---|---|
| `test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes` | send: stall `has_metadata_history_entry` | yes | call filter below |
| `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` | send: stall `retain_objects` | yes | call filter below |
| `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again` | send: stall `accept_metadata_commit` | yes | call filter below |
| `test_a_hung_health_probe_is_retried_inside_the_liveness_budget` | send: stall `ping` | no, Dannan's ping | G12, by holding the peer's ping handler |
| `test_a_ping_answered_while_the_health_pass_ran_late_keeps_its_route` | send: a slow sender | no; cannot occur in Dannan | G13 |
| | Closed by `Health.ALateAnswerKeepsTheRoute`, which holds a ping past its give-up and shows the route stays and the peer is measured. It does not force an answer to land between the give-up time and the timer firing; that branch is read, not exercised. | |
| `test_a_call_whose_dial_is_retired_by_a_simultaneous_connect_waits_for_the_peer` | admit: hold; send: retire first | no | G23, the Dannan fixture's `adopt_inbound` and a `Client` test hook, not a `NodeConfig` seam |
| `test_inbound_incapable_node_is_reached_only_over_its_own_sessions` | connect: sever, count; admit: count | no | `close_route`, `stats().dials`, `has_route` (G22) |
| `test_peer_sockets_disable_nagle_and_keep_alive` | connect: socket copies | no | G25 |
| `test_broadcast_notify_never_dials_and_never_waits_on_a_silent_peer` | connect: socket list | no | `stats().dials` (G21) |
| `test_dial_backoff_is_per_peer` | connect: count | no | `stats().dials`, already in `Backoff.AFailedDialWaitsOutItsBackoff` |

**The call filter is Macha's, not Dannan's.** All three stalled messages leave through Macha's own node: `metadata_manager.cpp:587` (`node_.call`), `:824` (`commit_call`), `distributed_store.cpp:835` (`bounded_control_call`). `ClusterNode::call` and `call_async` are virtual, stay as the facade, and return Macha's `AsyncRpc`. So `ServiceInstruments::links` becomes an instrument on the facade's call path that can hold a call by peer and type, the existing `stalled_call_locked` and `release` carrying over nearly unchanged. One thing to carry with it: the no-progress deadline on a held call must still fire, as `idle_for` makes it do today.

Stalling the peer's handler instead (`Routes::bind` replaces a bound handler, so it would work without any code) is worse for the commit case: `accept_metadata_commit` is ordered, so a held handler occupies the ordered worker and later asks queue unseen, and `CHECK(stalled_calls() == asked)` would pass for the wrong reason.

Other fixtures that look like seams and are not: the startup stage hook (`control-plane` is fired by the facade before `node.start()`; the storage stages are Macha's `LocalServices`); `telemetry_now` (see `test_status_renders_each_node_from_what_it_knows`: one `NodeConfig::time` replaces it, and the case adapts).

## Fixture nouns

What the replacement for `BareNode` (and `Service::node()`) must expose. Counts are regex hits of `.noun(` in case bodies at `00861dd`. The evaluation's 498 (at `5776392`) could not be reproduced: its five named nouns count 555 here, on any receiver, and its pattern is not recorded. "Remaining" excludes cases this mapping retires wholly or leaves to a gap, which is the number the fixture has to serve.

| Noun | All | Remaining | Comes from |
|---|---:|---:|---|
| `node` | 365 | 362 | Fixture itself; `Service::node()` returns the facade |
| `filesystem` | 345 | 345 | Macha; `Service`, `TestNode` |
| `local_state` | 247 | 247 | Macha: `LocalServices`, built by the fixture as `BareNode` does today |
| `catalogue` | 220 | 220 | Macha; `Service` |
| `membership` | 174 | 170 | `dannan::Node::membership()`; read-only uses can take `context().nodes()`. `observe` is needed (Status, manage API, store placement tests) |
| `node_id` | 82 | 81 | `context().identity().id()` |
| `call` | 82 | 30 | Facade over `context().routes().call_and_wait`, deadline from `no_progress_deadline_for` and the filter from Seams |
| `config` | 73 | 73 | Macha's `Config`, kept beside the `NodeConfig` it produces |
| `metadata_server` | 69 | 69 | Macha: `LocalServices::metadata` |
| `metadata_manager` | 52 | 52 | Macha; `Service` |
| `local_store` | 41 | 41 | Macha: `LocalState::data` |
| `resources.data` | 35 | 35 | Macha: `NodeResources`, the arbiter stays |
| `wait_local_state_ready` | 34 | 34 | Facade over `context().readiness().wait_complete` |
| `users` | 28 | 28 | Macha: `Accounts` |
| `resources.memory` | 27 | 27 | `Node::memory()`: one ledger, sized by `NodeConfig`, shared with Macha's store |
| `ledger` | 24 | 24 | Macha: `RetentionLedger`, built on first use as today |
| `resources.activity` | 21 | 21 | `context().activity()` |
| `resources.events` | 20 | 20 | `context().events()`; Macha registers its kinds |
| `control_store` | 19 | 19 | Macha: `LocalState::control` |
| `metadata_replica` | 18 | 18 | Macha: `LocalState::replica` |
| `known_metadata_generation` | 16 | 16 | Macha; the remote half becomes an advert |
| `accounts` | 15 | 15 | Macha: `Accounts`, which the fixture must build and start beside the node |
| `sessions` | 15 | 15 | Macha: `Accounts` |
| `work_stats` | 14 | 4 | `Node::work_stats()` |
| `inbound_capable` | 13 | 13 | `context().nodes().inbound_capable`, or `Node::inbound()` |
| `hosts_extents` | 13 | 13 | Macha's reader of its advert |
| `claims` | 11 | 11 | Macha: `LocalState::retention` |
| `rpc_stats` | 10 | 0 | `Node::client_stats()`: `connections_created` is `dials`. **`canonical_connections` has no equivalent**; only `test_mutual_bootstrap_prunes_cross_dial` uses this noun, and it retires to G7 |
| `block_cache` | 9 | 9 | Macha: `LocalState::cache` |
| `apply_identity_reset` | 8 | 1 | `Node::apply_identity_reset` |
| `telemetry` | 5 | 5 | `context().telemetry()`; Macha's fields read through its layout, ids from 1024 |
| `readiness` | 4 | 4 | `context().readiness()` |
| `durability_epoch` | 4 | 4 | `context().identity().durability_epoch()` |
| `identity_resets` | 3 | 1 | `context().nodes().identity_resets()` |
| `peer_latency` | 3 | 1 | `context().nodes().latency` |
| `inbound_resolution` | 3 | 3 | `Node::inbound().state()` |
| `propagate_session` | 3 | 3 | Macha: `Accounts` |
| `traffic` | 2 | 0 | `Node::traffic()` |
| `propagate_users` | 2 | 2 | Macha: `Accounts` |
| `request_lane` | 1 | 0 | gone: Dannan answers `dial_request` itself |

Beyond the nouns, the fixture needs: a **stage hook** (`BareNode`'s third argument, used by the recovering and startup cases); a **route table noun**, Macha's types registered from 512 with lane, frames and ordering, since `Routes::add` freezes at `start()` and the table is itself what several rewrites assert; a **bare peer** (a plain `dannan::Node` with bound handlers, replacing hand-built `RpcServer` peers in `test_storage_data_credit_reserves_viewer_headroom_and_control` and `test_service_shutdown_cancels_pending_outbound_rpc_before_join`); an **io context** and its threads, which `BareNode` does not have today; and **`FakeClusterNode` unchanged**, which keeps the store cases as they are provided `ClusterNode`, `AsyncRpc`, `RpcMessage` and `NodeInfo` survive as facade types. If they do not, those keep rows become rewrites.

### Store, metadata, FUSE, media and API cases that need the new fixture

Out of scope for fates; counted because they depend on the node. "Names the node" means `BareNode`, `TestNode`, `NodeRuntime` or `LocalServices` in the body or a helper it calls. "Through Service" means `Service`, `TestService` or `ServicesBench` only: `Service` owns a `NodeRuntime`, so these move with the facade and need the nouns, without constructing the fixture themselves. **133 name the node, 65 go through `Service`, together 198 of 617**; 23 use `FakeClusterNode`. The helper search is by name, within a file, so it can err by a case or two; `Service::required_role` alone was not counted.

| File | Names the node | Through Service | FakeClusterNode | Neither | All |
|---|---:|---:|---:|---:|---:|
| `test_availability.cpp` | 0 | 3 | 0 | 10 | 13 |
| `test_catalogue_maintenance.cpp` | 13 | 0 | 0 | 2 | 15 |
| `test_filesystem_fuse.cpp` | 16 | 0 | 0 | 1 | 17 |
| `test_fuse_subsystem.cpp` | 0 | 7 | 0 | 0 | 7 |
| `test_hydration_catalogue.cpp` | 39 | 0 | 0 | 5 | 44 |
| `test_invariants.cpp` | 13 | 0 | 0 | 18 | 31 |
| `test_lifecycle_record.cpp` | 0 | 2 | 0 | 0 | 2 |
| `test_maintenance_trace.cpp` | 7 | 0 | 0 | 3 | 10 |
| `test_media_playback.cpp` | 7 | 0 | 0 | 9 | 16 |
| `test_namespace_entries.cpp` | 1 | 1 | 0 | 6 | 8 |
| `test_namespace_migration.cpp` | 0 | 11 | 0 | 4 | 15 |
| `test_node_services.cpp` | 5 | 0 | 0 | 0 | 5 |
| `test_root_parts.cpp` | 0 | 0 | 3 | 0 | 3 |
| `test_rpc_cluster.cpp (store and metadata cases)` | 6 | 29 | 20 | 0 | 55 |
| `test_runtime_dependencies.cpp` | 0 | 1 | 0 | 12 | 13 |
| `test_service_root.cpp` | 0 | 8 | 0 | 0 | 8 |
| `test_session.cpp` | 5 | 0 | 0 | 5 | 10 |
| `test_storage_metadata.cpp` | 1 | 3 | 0 | 64 | 68 |
| `test_storage_v18.cpp` | 16 | 0 | 0 | 5 | 21 |
| `test_users.cpp` | 4 | 0 | 0 | 8 | 12 |

No dependence: `test_absent_nodes.cpp`, `test_baselines.cpp`, `test_book_conversion.cpp`, `test_claim_walk.cpp`, `test_epub_metadata.cpp`, `test_horizon_builder.cpp`, `test_http_compression.cpp`, `test_http_server.cpp`, `test_io_pressure.cpp`, `test_ledger_gates.cpp`, `test_metadata_merge.cpp`, `test_mobi_metadata.cpp`, `test_models.cpp`, `test_namespace_tree.cpp`, `test_object_ledger.cpp`, `test_predicates.cpp`, `test_presence.cpp`, `test_torrent_disk_io.cpp`, `test_transcode_timeline.cpp`, `test_web_api.cpp`.

## Gaps

What Dannan should add. The Macha cases each gap serves are in the rows. Names are proposals; none exists today.

| Gap | Case to add | Does Dannan's code already do it |
|---|---|---|
| G1 | PeerSocket.ChoosesBbrWhereOffered: both ends carry bbr when a plain socket may choose it; the return says so | Yes, server.cpp:42. Linux path |
| G2 | Client.ALargeTransferIsOvertakenCancelledAndPromotedEndToEnd: 32 MiB at speculative; a foreground call reaches the handler first; a promoted small call overtakes on both sides; a cancel mid-transfer leaves the session up with no new dial | Yes in parts (Outbound.*, Session.*, Scheduler.*); never together, and no Dannan case moves over 2 MiB. Overlaps item 6 of the parity list |
| G3 | Client.AFastCallPassesAHeldOneOnASession: one session, one held, one answered, then an empty call | Yes |
| G4 | Session.ALargeReplyLeavesTheSessionReusable: 4 MiB + 36 reply, twice, no new dial | Yes |
| G5 | Client.TheDialledNodeReusesTheDataLane: A opens DATA, B calls over it, B dials nothing | Yes (route_locked) |
| G6 | Client.ASecondNameForOnePeerEndsInOneRoute: numeric and alias endpoints | Yes, client.cpp:720-733 |
| G7 | Node.MutualSeedsSettleOnOneSessionAndStopDialling: count over a quiet window | Believed yes through settlement; unproven |
| G8 | Channel.AHelloWithNoCommonVersionIsRefused and Server.ClosesAHelloOfAnotherVersionUnanswered | Yes, channel.cpp:306-310 |
| G9 | Channel.AnAckOutsideTheRangeIsRefused (forged, correctly signed) | Yes, channel.cpp:197 |
| G10 | Channel.ANegotiatedFrameAboveTheLimitIsRefused (forged, correctly signed) | Yes, channel.cpp:201 |
| G11 | Health.ASlowCallOutlivesTheLivenessBudgetWhilePingsAnswer: both lanes, stepped or short budget, no route lost | Yes |
| G12 | Health.EveryHungPingIsRetriedAtAThirdOfTheBudget: three attempts inside the budget. fast_control_workers is 2, so count arrivals rather than held handlers | Yes, client.cpp:1220 |
| G13 |
| | Closed by `Health.ALateAnswerKeepsTheRoute`, which holds a ping past its give-up and shows the route stays and the peer is measured. It does not force an answer to land between the give-up time and the timer firing; that branch is read, not exercised. | | Health.ALateAnswerKeepsTheRoute: an answer read after the give-up time is proof of life. Macha's form (a blocked sender) cannot occur, since Session::call never blocks | Yes, client.cpp:1241 |
| G14 | Scheduler.HeldOrderedWorkDelaysNoOtherClass (add an ordered row); Scheduler.ATimingRecordsItsWaitAndHandlerTime; Server.ASessionOutlivesARefusal | Yes |
| G15 | Scheduler.AControlMutationOvertakesItsOwnPeersBackgroundWork: Macha's order 1, 5, 2, 3, 4 from one peer | Yes, scheduler.cpp:240 |
| G16 | Server.ALostSessionLeavesARunningJobToFinish: its reply dropped, nothing crashes | Believed yes; Session.ClosingFailsEveryPendingCallAndTellsBothSides covers only the late reply |
| G17 | Client.ASharedDialToASilentListenerEndsWithinTheConnectTimeout: accepts, never answers; the second caller is bounded too | Yes, client.cpp:657 |
| G18 | Client.ASlowCallInsideItsDeadlineSucceeds | Yes |
| G19 | Client.TheDataLanesDeadlineBindsOnlyDataCalls, and the converse | Yes (data_no_progress) |
| G20 | Broadcast.BestEffortAndNotifyTakeBothRouteDirections, speculative and control | Yes, one path: notify_routes |
| G21 | Broadcast.ABackedUpPeerIsSkippedWithoutWaiting: returns the count of the others; a known peer with no route is not dialled | Yes, session.cpp:258-263 |
| G22 | ReverseDial.TheHubUsesARedialledLaneWithoutAsking: dial_requests_sent stays put | Yes |
| G23 | Client.ACallOnADialRetiredBeforePlacementWaitsForThePeer: forced, with the fixture's adopt_inbound and a Client test hook like hold_routes_for_test | Partly: place() reroutes up to three times (client.cpp:598-603), which may dial again rather than wait. Unproven; read it when writing the case |
| G24 | Node.AnUnreachableAutomaticNodeGoesIncapableAndPeersStopDialling: real failures, persisted across restart, reversed once dialable | Believed yes; each part is proved alone |
| G25 | Client.ItsOwnSocketsKeepAliveWithoutNagle: both lanes | Yes, client.cpp:670. Needs the session's descriptor in a test |
| G26 | Node.TrafficAgreesAcrossTwoNodesByClass: 10,000 + 56 out equals in | Yes |
| G27 | Session.AFrameOffItsLaneEndsTheSessionUndispatched | Believed yes (session.cpp:88, :113); unproven |
| G28 | Membership.ANewerResetClearsAFreshAssociation: a new id at the endpoint accepted; an equal epoch refused | Believed yes |
| G29 | Membership.AnAddressWideResetDropsEveryNodeThereAndOnlyThem: later direct and gossip about a replacement accepted | Believed yes |
| G30 | Membership.AReloadedNodeFencesUntilHeardDirectly: all_known_reachable across a reload and after dead_after | Believed yes |
| G31 | Membership.AGossipedCapabilityChangeIsPersisted | Believed yes |
| G32 | Membership.ForgettingCountsFromLastHeard: the file forgets; gossip holds a node; a restart keeps the wait; no horizon forgets nobody | Believed yes |
| G33 | Membership.ARosterWriteHoldsNoReader: needs an fsync hold in Dannan's test binary, Linux only as in Macha | Yes by reading, membership.cpp:132 writes outside the lock |
| G34 | Placement.TheQuotaBoundaryIsExact: Macha's tag-built ids, 3,354,133 of 2^32 shards, first_small - 1 and first_small | Likely; by_capacity matched the reference set, which does not include this boundary |
| G35 | RetainedMemory.ReassemblyUsesItsReserveBesideALowerWaiter: 4 of 5 admitted, 4 more deferred, a non-frame owner deferred | Believed yes |
| G36 | Retry.ABackstopMeasuresOnlyTheCurrentRun, and with none a run spaced at the ceiling never parks (400 attempts; parks at 122 with 1 h) | Believed yes |
| G37 | DurableFile.KeepsEmptyAndBinaryContentsExactly | Yes |
| G38 | Waits.ComposeAndIncludeOverEveryMask; Waits.MayEnterIsASubsetOverEveryMask; Budget.StopPrecedenceOverEveryCombination; YieldSource destroyed through its interface | Yes |
| G39 | Published.RefusesNullAtConstructionAndWhenEmpty; Published.DestroysThePreviousOutsideItsLock; Published.ReadersSeeWholeSnapshotsInOrder | Believed yes |
| G40 | Observation: every bucket's width, exact below 8, every value to 2^20; quantile clamps and inconsistent snapshots; reversed since() saturates; records from many threads; histogram overflow; ObservationSnapshot::since; Macha's 13 route labels; rotation edges; an unwritable path logs once per failure run; a sampler exception absorbed, others propagated; final window, second stop writes nothing | Believed yes (observation.cpp:135, :167, :303); the rows are Macha's, so port them |
| G41 | Readiness.KeepsTheFirstFailureAndWakesItsWaiters | Yes by the header, readiness.h:43-55; no test calls them. The 2026-11-05 withdrawal of per-plane progress rests on this |
| G42 | SupervisedThreads.ALoopInBackoffStopsWhenAsked: inside the backoff; unknown_exception for a non-std throw | Believed yes |
| G43 | SupervisedThreads.AnEscalationNamesTheThread | **No.** supervised.cpp:84 passes the bare what(); Macha gives "thread 'name' faulted: ..." (src/supervised.cpp:167). supervise_test.cpp:277 pins "EIO" and must change. A subsystem's last_fault shows it |
| G44 | Supervisor.ThePluginsAreJournalled: "loaded and running", "not enabled on this node", with the path; Supervisor.AnEmptyDirectoryLoadsNone; Supervisor.StatusesEmptyAfterStop; Supervisor.AFaultedInstanceGoesBeforeItsReplacement | **No** for the lines: supervisor.cpp writes no log and supervisor.h takes no sink. The rest not checked |
| G45 | Supervise.EveryThreadIsRunSupervised: a source scan like Macha's | **No.** Bare: net/scheduler.cpp:37-40, reach/paths.cpp:525, status/observation.cpp:317, supervise/supervisor.cpp:77. A non-std throw out of the recorder's tick ends the process, where Macha's supervised loop records it |
| G46 | Telemetry.ATruncatedRecordIsRefused; Telemetry.AnUnknownFieldLeavesTheNextRecordWhole | Believed yes |
| G47 | Refusal.ATextOnlyErrorReadsAtStatusZero; add 410, 413, 507 to the not-again list | Yes, message.cpp:36-43 |
| G48 | Diagnostics.AHotThreadIsNoticed, with whatever Dannan's threads should report | **No.** Macha's net.cpp:2805, :4492, :4512, :4532, :4621 and cluster.cpp:961, :1080 run a ThreadCpuReporter; Dannan has none, and the parity list's journal table does not name them. Those DIAG lines go dark at cutover unless Tom rules otherwise |

## Gates by step

Each step of `ACTIVE.md`'s plan closes these gaps; the Macha cases are what each gap was drawn from, and the proposed Dannan case for each is in Gaps above.

| Step | Gaps | Macha cases |
|---|---|---|
| 1. Call semantics | G11, G12, G13, G18, G19 | `test_a_hung_health_probe_is_retried_inside_the_liveness_budget`, `test_a_ping_answered_while_the_health_pass_ran_late_keeps_its_route`, `test_no_progress_deadline_follows_the_lane_configured`, `test_rpc_call_fails_after_no_progress_deadline`, `test_rpc_slow_control_does_not_abort_data` |
| 2. Lifecycle and readiness | G16, G41, G42, G43, G45 | `test_a_supervised_loop_in_backoff_stops_when_asked`, `test_an_escalating_thread_hands_its_fault_to_the_sink`, `test_every_subsystem_thread_is_run_supervised`, `test_recovery_progress_keeps_the_first_failure_and_wakes_its_waiters`, `test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` |
| 3. Transport proofs | G1, G2, G3, G4, G5, G6, G7, G8, G9, G10, G14, G15, G17, G20, G21, G22, G23, G25, G26, G27 | `test_a_call_whose_dial_is_retired_by_a_simultaneous_connect_waits_for_the_peer`, `test_a_client_refuses_an_ack_of_another_version`, `test_a_hello_from_a_newer_protocol_is_refused`, `test_a_negotiated_frame_above_the_clients_limit_is_refused`, `test_a_promoted_call_overtakes_on_both_sides`, `test_authenticated_receiver_enforces_transport_lane`, `test_best_effort_telemetry_notifications_reach_both_route_directions`, `test_broadcast_notify_never_dials_and_never_waits_on_a_silent_peer`, `test_inbound_incapable_node_is_reached_only_over_its_own_sessions`, `test_mutual_bootstrap_prunes_cross_dial`, `test_peer_sockets_disable_nagle_and_keep_alive`, `test_peer_sockets_use_delay_based_congestion_control`, `test_rpc_a_dial_in_flight_is_waited_for_no_longer_than_a_dial`, `test_rpc_full_extent_reply_uses_owned_transport_handoff`, `test_rpc_metadata_control_mutations_go_first`, `test_rpc_metadata_mutations_use_bounded_isolated_executor`, `test_rpc_v15_bidirectional_and_deduplication`, `test_rpc_v15_frame_priority_and_variable_length`, `test_rpc_v15_persistence_and_multiplexing`, `test_rpc_v7_handshake_is_rejected`, `test_traffic_counts_both_directions_by_frame_class_in_one_instance` |
| 4. Membership proofs | G24, G28, G29, G30, G31, G32, G33 | `test_a_node_unheard_of_for_the_horizon_is_forgotten`, `test_a_roster_being_written_holds_no_reader_of_membership`, `test_forgetting_counts_from_when_the_node_was_last_heard_of`, `test_inbound_auto_resolves_from_dial_back_and_survives_restart`, `test_membership_identity_reset_tombstone`, `test_membership_ip_identity_reset_without_node_id`, `test_membership_persists_gc_fence_and_requires_direct_reachability`, `test_node_info_flags_travel_on_the_wire_and_in_the_roster` |
| 5. The journal | G44, G48 | `test_builtin_lifecycle_settles_each_outcome`, `test_plugin_discovery_settles_each_kind_of_file`, `test_thread_cpu_reporter_debug_escalation` |
| 6. Telemetry and Status | G46, G47 | `test_codec_and_crypto`, `test_rpc_refusals_say_whether_asking_again_may_help` |
| 8. Foundations proofs | G34, G35, G36, G37, G38, G39, G40 | `test_a_yield_source_is_destroyed_through_its_interface`, `test_budget_stop_precedence_over_every_combination`, `test_capacity_placement`, `test_durable_replace_file_matrix`, `test_observation_bucket_contains_every_small_value`, `test_observation_buckets_tile_the_value_range`, `test_observation_histogram_records_from_many_threads`, `test_observation_log_rotates_at_its_bound`, `test_observation_log_survives_an_unwritable_path`, `test_observation_quantiles`, `test_observation_recorder_thread_and_final_window`, `test_observation_recorder_writes_windows_and_events`, `test_observation_registry_is_bounded`, `test_observation_registry_windows`, `test_observation_route_labels`, `test_observation_snapshot_windows`, `test_publish_destroys_the_previous_snapshot_outside_its_lock`, `test_published_readers_see_whole_snapshots_in_order`, `test_published_refuses_a_missing_snapshot`, `test_retained_memory_reassembly_reserve_is_bounded_not_absolute`, `test_retry_budget_is_reachable_once_backoff_reaches_its_ceiling`, `test_waits_compose_and_include_over_every_mask`, `test_work_may_allow_itself_less_never_more` |

## The cases

### `test_rpc_cluster.cpp`

| Case | Subject | Fate |
|---|---|---|
| `test_peer_sockets_use_delay_based_congestion_control` | BBR chosen on both socket ends where offered | **gap**: G1 |
| `test_rpc_reassembly_has_count_byte_and_message_bounds` | reassembly bounds | **retire**: Assembler.BoundsTheIncompleteMessagesTheirBytesAndEachMessage, Assembler.JoinsFragmentsIntoOneMessage |
| `test_rpc_reassembly_is_process_memory_charged_until_consumed` | reassembly charged to the ledger until consumed | **retire**: Assembler.ChargesItsBytesUntilTheMessageIsGone, Assembler.RefusesWhatTheMemoryLedgerCannotHold |
| `test_async_rpc_move_ownership` | AsyncRpc moves its cancellation; destroying it cancels | **keep**: AsyncRpc stays as the ClusterNode facade's type, built on net::CallHandle, which has no cancel on destruction; the facade supplies it |
| `test_best_effort_telemetry_notifications_reach_both_route_directions` | best-effort notify, both route directions, two classes | **retire**: dialler to acceptor: Node.TheThreeBroadcastsReachPeers, Broadcast.BestEffortSkipsABusyRouteTableAndNotifyWaitsForIt<br>**gap**: G20: acceptor to dialler at best effort, and at control class |
| `test_rpc_v15_frame_priority_and_variable_length` | frame order and names; short final frame; overtaking; cancel mid-transfer | **retire**: FrameType.PriorityFollowsTheLaws, FrameType.NamesAreStable, Session.ALargeMessageCrossesInFragments, Outbound.AMessageLargerThanAFrameLeavesInFragments, Scheduler.RecordsTimingsPerTypeAndPerFrameClass, Health.TheRoundTripComesFromPingsNotFromSlowReplies<br>**rewrite**: Macha's types: default frame, names, put_control_object on CONTROL at speculative. Fixture: route table noun<br>**gap**: G2: the 32 MiB overtake and the cancel mid-transfer end to end |
| `test_rpc_v15_persistence_and_multiplexing` | one session, reused; slow call does not block a fast one | **retire**: Client.DialsALaneOnDemandAndReusesIt, Client.NoDeadlineWhenTheLanesIsZero, Journal.AStallWithNoDeadlineLeavesTheCallToFinish<br>**gap**: G3: a fast call answered past a held one on one session |
| `test_rpc_concurrent_cold_data_calls_share_one_dial` | 24 cold callers share a dial | **retire**: Client.ConcurrentColdCallersShareOneDial (placed from one thread, not 24; the dial table is under one lock) |
| `test_rpc_full_extent_reply_uses_owned_transport_handoff` | 4 MiB reply whole, session reusable | **gap**: G4: Session.ALargeMessageCrossesInFragments moves 1 MiB once and never reuses the session |
| `test_rpc_v15_bidirectional_and_deduplication` | call back, cross dial, alias, backoff, retirement drains | **retire**: Client.CallsBackOverTheSessionThePeerOpened, Client.SimultaneousDialsSettleOnTheLowerIdsSession, Backoff.NeverMasksARouteThePeerOpened, Session.RetiringAdmitsNothingNewAndClosesOnceDrained<br>**gap**: G5 (DATA lane reused by the dialled node), G6 (a hostname alias ends in one route) |
| `test_mutual_bootstrap_prunes_cross_dial` | mutual seeds settle, dialling stops | **gap**: G7: Node tests bootstrap one way only |
| `test_rpc_v7_handshake_is_rejected` | old-version hello closed | **obsolete**: the v7 hello bytes: wire v1 ruling (5.3 of the evaluation; CLAUDE.md: nothing of protocol 23 binds Dannan)<br>**gap**: G8: the behaviour, no common version closes the connection |
| `test_rpc_slow_control_does_not_abort_data` | slow calls outlive the liveness budget | **gap**: G11: Client.NoDeadlineWhenTheLanesIsZero runs inside the default budget and only on CONTROL |
| `test_rpc_request_payload_is_charged_until_handler_completion` | request charged until its handler returns | **retire**: Scheduler.ARequestIsChargedUntilItsHandlerReturns |
| `test_rpc_held_executors_do_not_delay_other_classes` | each class answers while another is held | **retire**: Scheduler.HeldWorkersOfOneClassNeverDelayAnother<br>**rewrite**: the have_objects row: its class is Macha's registration. Fixture: route table noun |
| `test_rpc_call_fails_after_no_progress_deadline` | stuck call fails, transient; zero means none | **retire**: Client.ACallWithNoProgressFailsAtTheLanesDeadline, Client.NoDeadlineWhenTheLanesIsZero<br>**gap**: G18: slow but inside the deadline succeeds (the Dannan comment claims it; the body does not assert it) |
| `test_rpc_metadata_mutations_use_bounded_isolated_executor` | ordered queue bounded and isolated; every class byte-bounded; timings | **retire**: Scheduler.AQueueOverItsByteBoundRefusesAndCounts, Scheduler.ControlClassOrderedWorkGoesFirstAndKeepsAQuarterOfTheQueue<br>**rewrite**: the three metadata types are ordered: Macha's registration. Fixture: route table noun<br>**gap**: G14: other classes served with the ordered worker held; sessions usable after a refusal; wait and handler timings |
| `test_rpc_metadata_executor_orders_each_peer_and_parallelises_peers` | per-peer order, peers side by side | **retire**: Scheduler.OrderedWorkKeepsEachPeersOrderAndRunsPeersSideBySide |
| `test_rpc_a_dial_in_flight_is_waited_for_no_longer_than_a_dial` | second caller bounded by the dial | **gap**: G17 |
| `test_rpc_job_traffic_travels_as_its_own_class` | job polls as loader, actions as foreground | **rewrite**: Macha's types and frames. Fixture: two nodes, route table noun, call |
| `test_rpc_metadata_control_mutations_go_first` | control mutation overtakes background, same peer | **retire**: queue share: Scheduler.ControlClassOrderedWorkGoesFirstAndKeepsAQuarterOfTheQueue<br>**gap**: G15: that case uses six peers; Macha's uses one, which is the commit's case |
| `test_rpc_metadata_executor_cancellation_disconnect_and_shutdown` | queued cancel, running job survives disconnect, stop | **retire**: Scheduler.ACancelledQueuedRequestNeverRuns, Scheduler.StoppingFinishesRunningWorkAndDropsTheQueue<br>**gap**: G16: a lost session leaves the running job to finish, reply dropped |
| `test_service_shutdown_cancels_pending_outbound_rpc_before_join` | Service::stop fails a pending call first | **retire**: transport half: Node.CancellingOutboundCallsReleasesAWaitingWorker, Client.StoppingFailsPendingCallsAtOnceAndRefusesNewOnes, Node.StoppingIsPromptAndCallsFailAfter<br>**rewrite**: Service::stop's order. Fixture: service, node().call, a bare peer node with a held handler |
| `test_data_credit_wait_gives_up_when_nothing_is_moving` | DATA arbiter no-progress | **keep**: DataResourceArbiter stays Macha's until the store moves |
| `test_data_credit_wait_survives_genuine_contention` | DATA arbiter under contention | **keep**: as above |
| `test_storage_data_credit_reserves_viewer_headroom_and_control` | viewer and control pass a full arbiter over the wire | **rewrite**: Fixture: node, local_store, resources.data, config; a bare peer node calling Macha's types (the case builds RpcClient and RpcServer by hand) |
| `test_startup_gate_fires_on_silence_not_on_slow_progress` | startup gate: silence, ceiling, poll | **retire**: StartupStallGate.StallsOnNoProgressNotOnSlowness |
| `test_service_startup_gate_spares_progress_and_ends_a_stall` | Service exits a stalled startup, spares a ticking one | **rewrite**: Fixture: Service with stage hook and stall handler over supervise::StartupWatch; note_startup_progress becomes the service's StartupProgress. Dannan's half: StartupWatch.AStalledStartupSaysSoAndIsEnded |
| `test_commit_replicas_ordered_local_then_nearest` | commit replica order | **retire**: Placement.CommitReplicasGoLocalFirstThenByRoundTrip |
| `test_a_ping_answered_while_the_health_pass_ran_late_keeps_its_route` | a late-read ping answer is proof of life | **gap**: G13 |
| `test_metadata_repair_stalled_on_a_silent_peer_does_not_block_local_writes` | repair stuck on a silent peer holds no write | **rewrite**: Macha-visible. Fixture: service pair, filesystem, metadata_manager, call filter (see Seams) |
| `test_a_commit_is_claimed_here_when_the_peer_never_answers_the_claim` | a silent peer does not hold a commit's claims | **rewrite**: Macha-visible. Fixture: service pair, catalogue, local_state, metadata_manager, call filter (see Seams) |
| `test_a_peer_that_stalls_on_a_commit_is_not_waited_for_again` | replicator waits once for a stalled peer | **rewrite**: Macha-visible. Fixture: service pair, filesystem, metadata_manager, call filter (see Seams) |
| `test_a_hung_health_probe_is_retried_inside_the_liveness_budget` | three ping attempts per budget | **retire**: route kept: Health.AHungPingIsRetriedInsideTheLivenessBudget<br>**gap**: G12: that case hangs one ping and counts no attempts |
| `test_inbound_incapable_node_is_reached_only_over_its_own_sessions` | incapable node: no dial, both ways, reverse DATA, redial, probe | **retire**: ReverseDial.AnIncapableNodeIsNeverDialled, ReverseDial.ControlTravelsBothWaysOverTheIncapableNodesSession, ReverseDial.ADataLaneIsAskedForAndDialledBack, Client.AProbeDialAuthenticatesAndLeavesNoRoute<br>**gap**: G22: after the lost lane is redialled, the hub's next DATA call uses it without asking |
| `test_a_call_whose_dial_is_retired_by_a_simultaneous_connect_waits_for_the_peer` | forced: dial retired before the call is placed | **gap**: G23: Client.SimultaneousDialsSettleOnTheLowerIdsSession only meets this gap by chance |
| `test_inbound_auto_resolves_from_dial_back_and_survives_restart` | auto inbound over a real cluster, persisted, reversed; founder refused | **retire**: InboundResolution.ConfiguredIsFinalAndAutoStartsFromWhatWasPersisted, InboundResolution.IsStickyAndFollowsTheCadence, ImpossibleCluster.IsRefusedWithAReason, Node.AnAutomaticNodeIsResolvedByDialBack<br>**rewrite**: hosts_extents follows the answer: Macha's tristate and advert. Fixture: node pair with restart, nodes().inbound_capable, the advert reader<br>**gap**: G24: the failing path end to end; Dannan's node case proves success only |
| `test_large_replies_never_crowd_out_a_small_message` | outbound reserve table | **retire**: OutboundRefusal.AnEmptyQueueTakesEither, OutboundRefusal.LargeMessagesStopShortOfTheSmallReserve |
| `test_peer_sockets_disable_nagle_and_keep_alive` | the transport's own sockets, both lanes | **gap**: G25: PeerSocket.KeepsAliveAtSixtySecondsWithoutNagle calls the option function on its own sockets |
| `test_peers_with_different_frame_limits_agree_on_the_smaller` | frame negotiation | **retire**: Channel.BothEndsAgreeOnTheSmallerFrameLimit, Frames.NothingLargerThanTheNegotiatedLimitIsSent |
| `test_a_negotiated_frame_above_the_clients_limit_is_refused` | forged ack above the client's limit | **gap**: G10 |
| `test_both_ends_of_a_session_share_its_id_and_no_other_session_does` | session ids | **retire**: Channel.BothEndsAgreeOnPeersLaneAndSessionId, Channel.NoTwoSessionsShareAnIdOrKeys |
| `test_a_tampered_fragment_ends_the_session` | tampered fragment refused | **retire**: Frames.ATamperedFragmentIsRefused |
| `test_a_replayed_fragment_ends_the_session` | replayed fragment refused | **retire**: Frames.AReplayedFragmentIsRefused |
| `test_a_peer_with_another_cluster_key_fails_the_handshake` | another cluster's key | **retire**: Channel.AnotherClustersKeyFailsBothWays, Server.RefusesAnotherClustersKey |
| `test_a_hello_from_a_newer_protocol_is_refused` | newer hello closed unanswered | **obsolete**: protocol 23 and 24 hello bytes: wire v1 ruling (5.3 of the evaluation; CLAUDE.md: nothing of protocol 23 binds Dannan)<br>**gap**: G8 |
| `test_a_client_refuses_an_ack_of_another_version` | ack of another version refused | **obsolete**: protocol 22, 23, 24 ack bytes: wire v1 ruling (5.3 of the evaluation; CLAUDE.md: nothing of protocol 23 binds Dannan)<br>**gap**: G9 |
| `test_traffic_counts_both_directions_by_frame_class_in_one_instance` | bytes by class across two nodes, one count per node | **retire**: Session.CountsTrafficByClassInBothDirections, Frames.CostFiftySixBytesOfOverhead<br>**gap**: G26: across two nodes, client and server in one count |
| `test_a_promoted_call_overtakes_on_both_sides` | promotion on both sides | **retire**: Scheduler.APromotedQueuedRequestRunsAtItsNewClass, Outbound.APromotedRequestOvertakesItsOldPeers, Session.APromotionReachesThePeer<br>**gap**: G2: the chain end to end |
| `test_broadcast_notify_never_dials_and_never_waits_on_a_silent_peer` | no dial, no wait on a backed-up peer | **retire**: Broadcast.NeverDials<br>**gap**: G21: a backed-up route skipped; a known unrouted peer not dialled |
| `test_a_ping_is_admitted_behind_a_full_queue_of_large_messages` | a ping fits behind large messages | **retire**: Outbound.ARequestPastTheLimitIsRefusedWithItsReason (at the queue, not over a silent peer) |
| `test_a_connection_holds_no_more_than_its_pending_reply_budget` | pending-reply budget; every call answered | **retire**: Session.HoldsNoMoreThanItsPendingReplyBudgetAndSaysWhy, Outbound.AReplyIsNeverDroppedForCapacity, Outbound.SubstitutesStopAtTheirOwnBound |
| `test_no_progress_deadline_follows_the_lane_configured` | each lane's deadline binds that lane | **retire**: CONTROL half: Client.ACallWithNoProgressFailsAtTheLanesDeadline, Client.ATypesOwnDeadlineReplacesItsLanes<br>**gap**: G19: the DATA half |
| `test_unauthenticated_connections_are_bounded_on_every_platform` | pre-auth bound | **retire**: Server.BoundsUnauthenticatedConnections |
| `test_round_trip_is_smoothed_from_pings_not_from_slow_replies` | round trip from pings | **retire**: Health.TheRoundTripComesFromPingsNotFromSlowReplies |
| `test_dial_backoff_is_per_peer` | backoff per peer | **retire**: Backoff.AFailedDialWaitsOutItsBackoff, Backoff.IsPerPeer |
| `test_dial_back_probes_are_rate_limited_per_asking_peer` | dial-back rate limit | **retire**: DialBack.IsRateLimitedPerAskingPeer (at the responder, not through a node) |
| `test_reprobe_cadence_defaults_follow_the_resolution` | Config's reprobe defaults | **keep**: Macha's Config defaults, passed to NodeConfig; the cadence itself is InboundResolution.IsStickyAndFollowsTheCadence |
| `test_a_reverse_dial_that_never_comes_fails_within_its_bound` | reverse dial bound | **retire**: ReverseDial.AReverseDialThatNeverComesFailsWithinItsBound |

### `test_foundations.cpp`

Cases not listed are out of scope and keep: `test_spool_retirement_rate_is_aggregate`, `test_weighted_loader_service_is_work_conserving_and_non_starving`, `test_data_resource_arbiter_background_effort_ceiling`, `test_data_resource_arbiter_reserves_viewer_headroom`, `test_torrent_uri_safety_matrix`, `test_config`, `test_normalize_config_defaults_plugin_path_to_the_build_default`, `test_publication_open_writer_bound_fits_the_loader_reserve`, `test_torrent_binds_the_advertised_address_not_libtorrent_enumeration`, `test_normalize_config_preserves_an_explicit_plugin_path`, `test_fuse_mountpoint_preflight_counts_stray_entries`, `test_fuse_mountpoint_preflight_refuses_unrelated_filesystem`, `test_configuration_tristates_and_hosting_rules`, `test_transcode_rates_keep_a_median_per_class_and_survive_a_restart`, `test_coverage_child_profile_name`, `test_write_behind_acts_once_per_window`, `test_write_behind_leaves_what_was_written_intact`.

| Case | Subject | Fate |
|---|---|---|
| `test_miniupnpc_igd_status_compatibility` | miniupnpc IGD status codes | **obsolete**: miniupnpc is replaced by the hand-written UPnP (5.3 of the evaluation) |
| `test_codec_and_crypto` | Writer, NodeInfo, telemetry codec, AES-GCM, X25519 | **keep**: Writer and Reader, crypto: Macha's store payloads and storage key stay Macha's<br>**obsolete**: MACHTEL3 and the NodeInfo bytes: Dannan's formats ruling (5.3)<br>**retire**: telemetry rules: Telemetry.FieldsAreTaggedAndDefaultsOmitted, Telemetry.RoundTripsAndKeepsFieldsItDoesNotKnow, Telemetry.RefusesWhatItCannotTrust, Telemetry.AMissingPhaseReadsAsReady; NodeRecord.RoundTrips<br>**rewrite**: every Macha telemetry field round-trips in the hosted layout (ids from 1024). Fixture: Macha's layout over status::Telemetry, no node<br>**gap**: G46: a truncated record; a set whose first record has an unknown field |
| `test_membership_identity_reset_tombstone` | tombstone: gossip fenced, direct returns, epochs | **retire**: Membership.IdentityResetsDropTheStaleMemberAndPersist<br>**gap**: G28 |
| `test_membership_identity_reset_is_durable_without_metadata` | tombstone survives restart | **retire**: Membership.IdentityResetsDropTheStaleMemberAndPersist, Membership.ThePersistedRosterReloads |
| `test_membership_persists_gc_fence_and_requires_direct_reachability` | a reloaded node fences until heard | **retire**: gossip half: Membership.AllKnownReachableExcludesPairsThatCannotMeet, Membership.DirectLivenessCountsAndGossipDoesNot<br>**gap**: G30 |
| `test_telemetry_identity_reset_freshness_boundary` | telemetry after a reset | **retire**: TelemetryStore.AResetDropsTheStaleRecordAndOldGossipStaysOut |
| `test_membership_ip_identity_reset_without_node_id` | reset by address alone | **retire**: matching only: IdentityReset.MatchesItsEndpointAndNode<br>**gap**: G29 |
| `test_durable_replace_file_matrix` | durable replace of empty and binary contents | **gap**: G37. While Macha's store keeps durable_file.cpp at the first cutover, this case stays beside it |
| `test_thread_cpu_reporter_debug_escalation` | per-thread CPU DIAG lines | **keep**: ThreadCpuReporter is Macha's for its own threads<br>**gap**: G48: Macha's transport threads report through it; Dannan's have no equivalent |
| `test_placement` | rendezvous deterministic, diverse | **retire**: Placement.RendezvousIsDeterministicAndPrefersDistinctDomains, Placement.MatchesMachasReferenceEverywhere |
| `test_capacity_placement` | logical capacity, shard quota, R=1 monotonic, domains | **retire**: Placement.LogicalCapacityHonoursTheCopies, Placement.ANewcomerOnlyTakesShardsItWins, Placement.CapacityPlacementListsEveryNodeOncePreferredFirst, Placement.MatchesMachasReferenceEverywhere<br>**gap**: G34: the exact quota boundary |
| `test_retained_memory_ledger_preserves_priority_headroom` | class headroom | **retire**: RetainedMemory.ControlAloneMayUseItsReserve, RetainedMemory.LowerClassesLeaveTheForegroundReserve, RetainedMemory.SpeculativeLeavesTheLoaderReserve, RetainedMemory.ALeaseReleasesItsBytesWhenItGoes, RetainedMemory.CountsBytesPerOwner |
| `test_retained_memory_ledger_never_starves_rpc_reassembly` | reassembly past the durable-lower budget | **retire**: RetainedMemory.ReassemblyIsNeverHeldBehindTheDurableLowerBudget |
| `test_retained_memory_reassembly_reserve_is_bounded_not_absolute` | reassembly reserve beside waiters | **retire**: viewer half: RetainedMemory.ReassemblyStandsAsideForWaitingForegroundWork<br>**gap**: G35: the lower-waiter half |
| `test_retained_memory_ledger_sheds_borrowed_cache_for_viewer` | shedding | **retire**: RetainedMemory.AHigherClassMakesReclaimableLowerMemoryShed |
| `test_retained_memory_ledger_restores_durable_overcommit` | restore overcommits | **retire**: RetainedMemory.RestoreIsNeverRefusedAndBlocksNewAdmissions |
| `test_every_subsystem_thread_is_run_supervised` | source scan: every thread supervised | **keep**: it scans Macha's src/<br>**gap**: G45: Dannan has no such guard and has bare threads |
| `test_supervised_restart_delay_doubles_to_its_ceiling` | restart delay | **retire**: SupervisedThreads.RestartDelaysDoubleToTheCeiling |
| `test_a_supervised_loop_that_throws_runs_again` | loop reruns, fault on record | **retire**: SupervisedThreads.RecordAndRestartAsTheirKindSays |
| `test_a_supervised_loop_in_backoff_stops_when_asked` | stop wakes the backoff; unknown_exception | **gap**: G42 |
| `test_a_supervised_task_that_throws_ends_on_record` | task ends on record | **retire**: SupervisedThreads.RecordAndRestartAsTheirKindSays |
| `test_an_escalating_thread_hands_its_fault_to_the_sink` | escalation names the thread and the fault | **gap**: G43: Dannan differs, and its case pins the difference |
| `test_retry_budget_is_reachable_once_backoff_reaches_its_ceiling` | density, backstop, flapping | **retire**: Retry.ParksPastTheFailureBudgetInTheWindow, Retry.ParksAnUnbrokenRunPastTheBackstop, Retry.ASuccessResetsTheBackoffButKeepsTheWindow<br>**gap**: G36 |
| `test_node_info_flags_travel_on_the_wire_and_in_the_roster` | capabilities on the wire and in the roster | **retire**: NodeRecord.RoundTrips, Membership.ThePersistedRosterReloads, Membership.UnknownNodesReportCapableAndSelfReportsItsOwnCapabilities<br>**rewrite**: hosts_extents as an advert (5.3). Fixture: Macha's advert helper over NodeRecord<br>**gap**: G31 |
| `test_gc_fence_excludes_pairs_that_can_never_dial_each_other` | pairs that cannot meet are no fence | **retire**: Membership.AllKnownReachableExcludesPairsThatCannotMeet |
| `test_message_routes_answer_only_while_bound` | 501 unbound, 500 on a throw, text first | **retire**: Node.RoutesBindAndUnbindWhileRunning, Scheduler.AHandlerSaysWhetherItProcessedFailedOrDidNotRecognise, Scheduler.AThrowingHandlerAnswersAndTheWorkerGoesOn, Refusal.TheTextComesFirst |
| `test_rpc_refusals_say_whether_asking_again_may_help` | try-later table | **retire**: Refusal.AnErrorReplyCarriesItsStatusCodeAndText, Refusal.OnlySomeStatusesMayBeTriedAgain, Refusal.AnEmptyOrForeignErrorStillReads<br>**gap**: G47: 410, 413, 507; a text-only refusal |
| `test_node_events_count_each_kind` | event counts | **retire**: NodeEvents.CountsPerKindAndWakesAWaiter |
| `test_a_connectivity_probe_holds_no_reader_of_the_status` | status does not wait on a probe | **retire**: PublicConnectivity.StatusNeverWaitsOnAProbe, PublicConnectivity.TheSelfProbeConnectsToTheAdvertisedEndpoint |
| `test_a_roster_being_written_holds_no_reader_of_membership` | a roster fsync holds no reader | **gap**: G33 |

### `test_contract.cpp`

| Case | Subject | Fate |
|---|---|---|
| `test_waits_compose_and_include_over_every_mask` | waits over every mask | **gap**: G38 |
| `test_only_control_is_refused_and_only_device_or_network_waits` | what each class may wait on | **retire**: Waits.ControlMayWaitOnlyOnThisNode, Waits.MayEnterOnlyWhatEveryDeclaredWaitAllows |
| `test_work_may_allow_itself_less_never_more` | narrowing, never widening | **retire**: WorkContext.AllowedNarrowsTheClassDefaultAndNeverWidens<br>**gap**: G38: the every-mask table |
| `test_wait_guard_records_or_throws` | guard records once per path, or throws | **retire**: WaitGuard.AdmitsWhatTheWorkAllows, WaitGuard.CountsEveryRefusalAndReportsEachPathOnce, WaitGuard.ThrowsOnARefusalWhenTestsAskIt |
| `test_work_context_deadline_and_cancellation` | deadline, cancellation | **retire**: WorkContext.ReportsCancellationAndExpiryAtTheTimeItIsGiven, WorkContext.NoDeadlineNeverExpires |
| `test_data_work_context_is_the_data_specialisation` | DataWorkContext | **keep**: cluster/data_work.hpp is Macha's; Dannan has none |
| `test_budget_bounds_are_spent_exactly` | budget spending | **retire**: Budget.AnAbsentBoundNeverStopsAWalk, Budget.OperationsAreSpentOneAtATimeAndThenRefused, Budget.ARefusedByteTakeSpendsNothing |
| `test_a_yield_source_is_destroyed_through_its_interface` | virtual destructor | **gap**: G38: walk.h:49 has it; nothing asserts it |
| `test_budget_stop_precedence_over_every_combination` | stop precedence, all 16 combinations | **retire**: order: Budget.MustStopNamesTheFirstReasonInOrder, Budget.TheContextsDeadlineStopsAWalkWithoutItsOwn<br>**gap**: G38: the table |
| `test_cursor_and_page` | cursor, page completeness | **retire**: Page.IsCompleteOnlyWhenTheWalkReachedTheEnd |
| `test_published_is_empty_until_the_first_publish` | empty until published | **retire**: Published.IsEmptyBeforeTheFirstPublish, Published.AHeldHandleKeepsItsSnapshot |
| `test_published_refuses_a_missing_snapshot` | null publish refused | **retire**: Published.RefusesToWithdrawTheSnapshot<br>**gap**: G39: null at construction, and on an empty one |
| `test_a_held_handle_keeps_its_snapshot_across_republish` | handles across republish | **retire**: Published.AHeldHandleKeepsItsSnapshot, Published.TheOldSnapshotLivesUntilItsLastHolderLetsGo |
| `test_publish_destroys_the_previous_snapshot_outside_its_lock` | destruction outside the lock | **gap**: G39: Published.TheOldSnapshotLivesUntilItsLastHolderLetsGo says it in a comment and does not test it |
| `test_published_readers_see_whole_snapshots_in_order` | racing readers | **gap**: G39 |
| `test_the_wait_guard_on_snapshot_views` | guard on Macha's real operations | **rewrite**: Fixture: service, metadata_manager, catalogue, the node's wait_guard in place of the static mode |
| `test_the_wait_guard_on_a_cold_catalogue` | guard on a cold catalogue | **rewrite**: Fixture: node, local_state, metadata_server, ledger, resources.*, wait_guard |

### `test_work_class.cpp`

| Case | Subject | Fate |
|---|---|---|
| `test_every_frame_has_one_class` | frame to class | **retire**: WorkClass.ComesFromTheFrameTypeAlone |
| `test_each_class_moves_data_on_its_own_frame` | class to frame | **retire**: WorkClass.FrameForRoundTripsThroughWorkClass |
| `test_presence_is_per_class_and_bytes_per_frame` | activity clocks | **retire**: ActivityClocks.PresenceIsPerClassAndBytesPerFrame |
| `test_control_never_forms_data_work` | DataWorkContext refuses control | **keep**: Macha's DataWorkContext |

### `test_observation.cpp`

| Case | Subject | Fate |
|---|---|---|
| `test_observation_buckets_tile_the_value_range` | every bucket: width and tiling | **retire**: contiguity: Observation.BucketsAreLogLinearAndContiguous<br>**gap**: G40: exact below 8, width on every bucket |
| `test_observation_bucket_contains_every_small_value` | every value to 2^20, each power of two | **gap**: G40 |
| `test_observation_quantiles` | quantile clamps and inconsistent snapshots | **retire**: basics: Observation.QuantilesAndWindows<br>**gap**: G40 |
| `test_observation_snapshot_windows` | window arithmetic | **retire**: Observation.QuantilesAndWindows<br>**gap**: G40: reversed operands saturate |
| `test_observation_histogram_records_from_many_threads` | concurrent records | **gap**: G40 |
| `test_observation_registry_is_bounded` | series bound | **retire**: Observation.TheRegistryIsBounded<br>**gap**: G40: the histogram overflow series |
| `test_observation_events_are_bounded_oldest_first` | event bound | **retire**: Observation.TheRegistryIsBounded |
| `test_observation_registry_windows` | registry since() | **gap**: G40 |
| `test_observation_process_registry_and_durations` | durations; the process registry | **retire**: ObservedDuration.TimesASpanOnTheInjectedClock<br>**rewrite**: the process-wide observations() becomes context().observations(); Macha's call sites. Fixture: observations noun |
| `test_observation_route_labels` | API route labels, 13 rows | **retire**: Observation.RouteLabelsAreBounded<br>**gap**: G40: Dannan has 4 of the 13 rows |
| `test_observation_window_rendering` | window line | **retire**: Observation.AWindowRendersAsOneJsonLine |
| `test_observation_event_rendering` | event line | **retire**: Observation.AWindowRendersAsOneJsonLine |
| `test_observation_log_rotates_at_its_bound` | rotation edges | **retire**: Observation.TheLogRotates<br>**gap**: G40: oversize line, empty file, newline counted |
| `test_observation_log_survives_an_unwritable_path` | unwritable path, one line per failure run | **gap**: G40 |
| `test_observation_recorder_writes_windows_and_events` | recorder ticks; a failing sampler | **retire**: Observation.TheRecorderWritesEventsThenTheWindow<br>**gap**: G40: sampler faults |
| `test_observation_recorder_thread_and_final_window` | thread, final window, second stop | **retire**: Node.ObservationsAreWrittenFromTheStart<br>**gap**: G40 |
| `test_a_service_writes_its_lifecycle_to_the_observation_file` | start, services_ready, shutdown in the file | **rewrite**: Macha's events and gauges. Fixture: service, config.state_path, observation_gauges |

### `test_subsystem_supervisor.cpp`

| Case | Subject | Fate |
|---|---|---|
| `test_plugin_discovery_settles_each_kind_of_file` | each file kind; journal lines | **retire**: Supervisor.LoadsPluginsAndRefusesAnotherBuild, Supervisor.AMissingPluginDirectoryLoadsNone<br>**gap**: G44: the INFO lines, an empty directory, statuses empty after stop |
| `test_builtin_lifecycle_settles_each_outcome` | builtin lifecycle, journal order | **retire**: Supervisor.RunsABuiltinAndStopsIt, Supervisor.ADeclinedSubsystemIsUnavailable, Supervisor.AFailingSubsystemIsRetriedThenDisabled, Supervisor.AFaultAfterStartRebuildsTheInstance<br>**gap**: G44: stopped and destroyed before the replacement is built |
| `test_stop_cancels_a_blocked_factory` | stop cancels a factory | **retire**: Supervisor.StopCancelsABlockingConstruction |
| `test_service_stops_plugins_before_the_store` | Service::stop's order | **rewrite**: Fixture: service, config.plugin_path. Dannan's half: Supervisor.APluginsStopMayWrite |

### `test_absent_nodes.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_a_node_unheard_of_for_the_horizon_is_forgotten` | forgetting at the horizon | **retire**: Membership.ForgetsAtTheHorizonAndOldGossipDoesNotTeachBack<br>**gap**: G32: the file forgets too |
| `test_forgetting_counts_from_when_the_node_was_last_heard_of` | gossip holds it; restart keeps the wait | **gap**: G32 |

### `test_models.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_capacity_placement_invariants_model` | placement invariants, R=1 to 6 | **retire**: Placement.CapacityPlacementListsEveryNodeOncePreferredFirst, Placement.ANewcomerOnlyTakesShardsItWins, Placement.MatchesMachasReferenceEverywhere |

### `test_invariants.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_rpc_pre_auth_admission_is_bounded` | idle sockets cost no thread each | **retire**: Server.BoundsUnauthenticatedConnections (async: no thread per connection exists) |
| `test_authenticated_receiver_enforces_transport_lane` | off-lane frame ends the session undispatched | **retire**: header refusal: Frames.AMessageOffItsLaneOrClassIsRefusedOnItsHeader<br>**rewrite**: get_object on the DATA lane: Macha's registration. Fixture: route table noun<br>**gap**: G27: the session ends, nothing dispatched |
| `test_service_answers_health_and_status_through_startup` | health and Status through startup | **rewrite**: Fixture: Service with stage hook (Macha's facade fires control-plane before node.start()), HTTP, accounts |
| `test_a_recovering_node_answers_peers_and_is_shown_recovering` | recovering node answers, phase shown | **rewrite**: Fixture: node with stage hook, readiness, call, membership, telemetry, resources.*, accounts |
| `test_status_is_light_and_diagnostics_have_their_own_route` | Status and diagnostics split; thread faults | **rewrite**: Fixture: node, resources.*, accounts; threads through context().threads() |
| `test_status_renders_each_node_from_what_it_knows` | Status per node | **rewrite**: Fixture: node with stepped time, membership().observe, apply_identity_reset, telemetry, local_state, metadata_server. NodeConfig::time is one source; the telemetry-only clock goes, so the case steps with a long dead_after |
| `test_a_node_advertises_its_api_endpoint_and_playback_budgets` | endpoint and budget functions | **keep**: pure functions of Macha's config |
| `test_status_follows_a_peer_through_telemetry_and_departure` | Status follows a peer | **rewrite**: Fixture: node pair, membership, telemetry with Macha's hosted fields, resources.*, accounts |
| `test_identity_association_reset` | manage API resets | **rewrite**: Fixture: CatalogueBench over node, membership().observe, identity_resets |

### `test_root_parts.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_recovery_progress_keeps_the_first_failure_and_wakes_its_waiters` | first failure kept, waiters woken | **gap**: G41: Node cases never call fail or wait_complete |
| `test_node_resources_yield_the_loader_to_a_recent_viewer_under_pressure` | viewer clock gates the loader | **rewrite**: NodeResources takes the node's activity clocks. Fixture: resources.activity, resources.data, stepped time |

### `test_service_root.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_a_failed_startup_is_what_every_caller_is_told` | failed startup reported | **rewrite**: Fixture: Service with stage hook, readiness in place of RecoveryProgress |
| `test_a_failed_startup_with_no_reason_still_says_it_failed` | failure with no reason | **rewrite**: as above |
| `test_a_stall_before_the_control_plane_names_it_starting` | stall names the stage | **rewrite**: Fixture: Service with stage hook and stall handler over supervise::StartupWatch |
| `test_a_stalled_startup_with_nobody_to_tell_ends_the_process` | no handler: exit 1 | **rewrite**: as above, forked. Also the only proof of StartupWatch's real _Exit(1) |

### `test_runtime_dependencies.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_memory_bytes_is_one_budget` | runtime.memory_bytes shrinks the parts | **keep**: Macha's YAML and part names; the parse delegates to dannan::fit_memory_limit. Dannan's half: MemoryLimit.OverTheLimitEveryPartAndReserveShrinksByTheSameFraction, Node.TheMemoryLimitShrinksEveryPartAndWarnsOnce |

### `test_baselines.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_baseline_observation_probe_costs` | probe cost baseline | **rewrite**: the baseline now measures status::Observations; no node. Fixture: Observations, route label |

### `test_node_services.cpp`

Only the cases in scope; the rest of the file is store, API or media.

| Case | Subject | Fate |
|---|---|---|
| `test_observation_gauges_follow_activity_repair_and_the_mount` | Macha's gauges | **rewrite**: Fixture: ServicesBench, resources.*, registry; delivered through NodeConfig::observation_gauges |

### `test_rpc_cluster.cpp`: store and metadata cases

Out of scope, sorted by fixture, listed so the file's 114 are all accounted for. The noun list is what the rewrite will reach for.

| Case | Fate | Fixture and nouns |
|---|---|---|
| `test_store_put_is_loader_activity_not_viewer_activity` | keep | FakeClusterNode over ClusterNode |
| `test_store_put_publishes_at_the_floor_and_copies_promptly` | keep | FakeClusterNode over ClusterNode |
| `test_store_put_replaces_a_replica_that_cannot_be_launched` | keep | FakeClusterNode over ClusterNode |
| `test_store_claims_ask_nothing_more_about_objects_nobody_holds` | keep | FakeClusterNode over ClusterNode |
| `test_store_put_to_a_silent_replica_returns_on_the_local_copy` | keep | FakeClusterNode over ClusterNode |
| `test_store_concurrent_readers_share_one_fetch_and_buffer` | keep | FakeClusterNode over ClusterNode |
| `test_repair_push_probes_by_window_and_sends_together` | keep | FakeClusterNode over ClusterNode |
| `test_a_long_push_pass_does_not_hold_back_the_pull` | keep | FakeClusterNode over ClusterNode |
| `test_repair_push_sends_nothing_to_a_peer_that_cannot_say` | keep | FakeClusterNode over ClusterNode |
| `test_repair_pushes_past_a_peer_with_no_room` | keep | FakeClusterNode over ClusterNode |
| `test_repair_pushes_from_what_peers_hold_without_asking` | keep | FakeClusterNode over ClusterNode |
| `test_repair_pushes_only_what_the_list_names` | keep | FakeClusterNode over ClusterNode |
| `test_repair_probes_without_credit_and_transfers_only_with_it` | keep | FakeClusterNode over ClusterNode |
| `test_repair_pass_keeps_its_place_across_live_sets_and_restarts` | keep | FakeClusterNode over ClusterNode |
| `test_repair_bandwidth_estimate_ignores_small_transfers` | keep | FakeClusterNode over ClusterNode |
| `test_repair_decides_already_held_without_reading_the_extent` | keep | FakeClusterNode over ClusterNode |
| `test_repair_keeps_a_pull_that_was_in_flight_when_its_turn_ended` | keep | FakeClusterNode over ClusterNode |
| `test_store_heals_a_bad_local_copy_from_a_peer` | keep | FakeClusterNode over ClusterNode |
| `test_store_runtime_cache_keeps_a_playback_fetch` | keep | FakeClusterNode over ClusterNode |
| `test_store_control_objects_take_no_data_credit_and_commit_alone` | keep | FakeClusterNode over ClusterNode |
| `test_torrent_listing_is_served_from_memory_never_a_survey` | rewrite | BareNode or TestNode: filesystem |
| `test_repair_is_paced_not_stopped_while_higher_classes_stay_busy` | rewrite | Service: local_state, membership, telemetry, wait_local_state_ready |
| `test_joiner_cannot_form_genesis` | rewrite | Service: filesystem, local_state |
| `test_bootstrap_joiner_requires_complete_checkpoint_survey` | rewrite | BareNode or TestNode: local_state, membership, metadata_replica, metadata_server, wait_local_state_ready |
| `test_nodes_seeking_different_copy_counts_share_one_namespace` | rewrite | Service: filesystem, membership |
| `test_two_node_mutual_bootstrap_metadata_write_floor` | rewrite | Service: filesystem, local_state, membership, metadata_server |
| `test_service_metadata_repair_coalesces_real_generation_burst` | rewrite | Service: filesystem, local_state, membership, metadata_server |
| `test_failed_convergence_retries_once_per_burst_of_events` | rewrite | Service: filesystem, local_state, membership |
| `test_an_ingest_interrupted_by_its_node_stopping_resumes` | rewrite | Service: filesystem |
| `test_an_ingest_job_read_elsewhere_carries_its_owners_catalogue_items` | rewrite | Service: membership |
| `test_service_same_generation_sibling_notice_triggers_reconciliation` | rewrite | Service: filesystem, local_state, membership, metadata_server |
| `test_a_head_from_an_unknown_history_is_merged` | rewrite | BareNode or TestNode: local_state, metadata_replica, metadata_server, wait_local_state_ready |
| `test_a_head_that_cannot_be_merged_is_set_aside` | rewrite | Service: filesystem, local_state, metadata_manager, metadata_server |
| `test_concurrent_reads_during_divergence_produce_one_reconciliation` | rewrite | BareNode or TestNode: local_state, membership, metadata_replica, metadata_server, wait_local_state_ready |
| `test_lagging_third_replica_catches_up_linear_burst_in_bounded_runs` | rewrite | Service: filesystem, local_state, membership, metadata_manager |
| `test_an_ingest_on_a_node_with_no_namespace_yet_resumes_when_it_has_one` | rewrite | Service: filesystem |
| `test_a_file_touch_commits_while_a_holder_is_away` | rewrite | Service: claims, filesystem, local_state, membership, metadata_manager |
| `test_a_delete_is_reclaimed_while_a_node_is_away` | rewrite | Service: filesystem, local_state, membership, metadata_manager |
| `test_retained_missing_copy_repairs_without_namespace_reachability` | rewrite | Service: filesystem, local_state, membership |
| `test_held_retention_claims_cost_repair_no_credit` | rewrite | Service: local_state, membership, wait_local_state_ready |
| `test_disjoint_metadata_pairs_branch_and_reconcile` | rewrite | Service: filesystem, local_state, membership, metadata_server |
| `test_replication_policy_change_on_restart` | rewrite | Service: filesystem, local_state, membership, metadata_server |
| `test_replacement_node_recovers_namespace_and_replication` | rewrite | Service: filesystem, local_state, membership, metadata_server |
| `test_a_durable_trio_founds_commits_and_reads_through_every_node` | rewrite | Service: filesystem |
| `test_metadata_commits_with_whichever_nodes_remain` | rewrite | Service: filesystem |
| `test_unlink_retires_an_object_from_the_maintenance_inventory` | rewrite | Service: filesystem |
| `test_rename_of_a_file_onto_a_directory_is_eisdir` | rewrite | Service: filesystem |
| `test_a_joining_node_pulls_its_objects_through_maintenance` | rewrite | BareNode or TestNode: filesystem, local_state, membership |
| `test_a_catalogue_conflict_decided_for_the_root_in_place_is_removed` | rewrite | Service: catalogue, local_state, metadata_manager |
| `test_every_nodes_unmatched_files_are_listed_and_handled_anywhere` | rewrite | Service: catalogue, filesystem, membership, telemetry |
| `test_ingest_job_pinned_to_a_node_runs_there` | rewrite | Service: membership |
| `test_ingest_torrent_jobs_visible_and_actionable_from_non_owning_node` | rewrite | Service: membership |
| `test_a_node_truncates_its_history_with_its_peer_away_and_the_peer_converges` | rewrite | Service: filesystem, local_state, membership, metadata_manager |
| `test_history_is_not_truncated_while_two_heads_are_held` | rewrite | BareNode or TestNode: local_state, metadata_replica, metadata_server, wait_local_state_ready |
| `test_unreconstructable_accepted_head_is_repaired_live_from_a_peer` | rewrite | Service: filesystem, local_state, membership, metadata_manager |
