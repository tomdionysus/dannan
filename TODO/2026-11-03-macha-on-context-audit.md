# Macha on dannan::Context: the audit

**Baseline moved, 2026-11-05: Macha `develop` at `cac5998`, tag 0.92.4 on `70cf4a4`.** Citations below were read at `00861dd`. In the moving files only `src/cluster/cluster.cpp` (from about `:225`), `cluster.hpp` (from `:122`), `net.cpp` (one line moves at `:2304`, a net shift of one after it) and comments in `src/types.hpp` and `src/config.hpp` changed, so those line numbers are off by a few, not re-cited. Read a copy at `build/scratch/macha-cac5998`.

Status: re-based 2026-11-05 on Macha at `00861dd` (0.92.3), and marked against
`dannan/context.h` as it now stands on `feature/macha-parity`.

Tom, 2026-11-03: Macha is the first customer; the design is not complete
until Macha can be refactored to use it. This lists every call Macha makes
into its network layer from outside it (`src/` minus `cluster/net.*`,
`cluster/cluster.*`, `membership`, `placement`, `telemetry`,
`public_connectivity`, `activity_clocks`, `node_events`, `node_identity`,
`observation`, `supervised`, `subsystem/`), and what each maps to in the
Context. The counts are a script's over the tree at `00861dd`, on the
receivers `node`, `node_` and `n_`, which is wider than the first count at
`6252dba`; they say how much use there is and no row turns on one. The new
uses since then are books reading `node_.config()`. No call was added to the
network layer, and none removed.

**closed** means `context.h`, or the node beside it, has what the row needs.
**open** means it does not yet. **Macha's** means it stays in Macha.

| Macha call (count at `00861dd`) | The Context now | Verdict |
|---|---|---|
| `node.config()` (150) | Macha's own config | **Macha's**, stays |
| `node.node_id()` (96) | `Identity::id` | **closed** |
| `node.membership().active()` (26), `.all()` (5) | `Nodes::active`, `roster` | **closed** |
| `membership().snapshot()` (2) | `Nodes::snapshot` (all and active in one read) | **closed** |
| `membership().all_known_reachable()`, `directly_reachable()` (1 each) | `Nodes::all_known_reachable`, `directly_reachable` | **closed** |
| `node.call(node or endpoint, type, payload[, frame])` (19), blocking, stall notice and no-progress deadline | `Routes::call_and_wait` (waits for the deadline at most; never on an io thread) | **closed** |
| `node.call_async` (12) and `AsyncRpc`: `get`, `wait_for`, `cancel` (4), `abort` (2), `promote` (1), `idle_for` (20, some of them `ActivityClocks`) | `Routes::call` returns a `net::CallHandle` with cancel, abort, promote, idle_for; the outcome comes to `done`, or from `call_and_wait` | **closed** |
| `RpcReply.peer` | `CallOutcome::peer` | **closed** |
| `routes_.bind/unbind` at runtime (storage server, metadata server, accounts, torrent and ingest plugins, availability; unbind waits for calls in flight) | `Routes::bind`, `unbind` at any time; `add` registers a type's wire rules before start | **closed** |
| `MessageRoutes::dispatch` answers 501 for an unbound type | `Handled::unrecognised`; a type with no handler answers 501 | **closed** |
| `node.broadcast_notify` (5), `broadcast` (metadata notice, `cluster/cluster.cpp:743`), `broadcast_best_effort` (telemetry, `cluster/cluster.cpp:985`) | `Channels::notify`, `broadcast`, `broadcast_best_effort` | **closed** |
| `node.telemetry().views/all/persisted/persist` (6), `peer_viewers_active` | the `Telemetry` noun: `local`, `all`, `views`, `persisted`, `persist`, `refresh`. `peer_viewers_active` is not on it: Macha's facade reads it from `views` and the traffic by class (parity item 7) | **closed** |
| `node.readiness()` (3), `wait_local_state_ready`, `RecoveryProgress` planes | `Context::readiness` (`status::Readiness`: `has`, `complete`, `failed`, `error`, `ready_unix_ms`, `wait_complete`); `Diagnostics::started_unix_ms`. Per-plane progress is settled as not needed: Macha's `RecoveryProgress` has none (`cluster/local_state.hpp:36`), and the ticking counter is `StartupProgress` | **closed** |
| `node.apply_identity_reset` (3), `propagate_identity_reset` (2), `identity_resets()` (2) | on `Nodes` | **closed** |
| `node.advertise_storage/backends/cache/metadata_generation`, `publish_self`, `announce_metadata_generation`, `remote_metadata_generation/epoch` | `Nodes::advertise(key, bytes)`, `withdraw`: a change makes the record newer, so gossip carries it. The generation announcements become a Macha notification type over `Channels::broadcast` plus `events` | **closed** |
| `node.rpc_stats`, `rpc_server_work_stats`, `public_connectivity_status`, `refresh_public_connectivity`, `peer_latencies`, `inbound_resolution`, `traffic_totals` (Status API) | `Context::diagnostics()` (`client`, `work`, `connectivity`, `inbound`, `traffic`), `Nodes::latencies`, `Nodes::refresh_public_connectivity` | **closed** |
| `node.signal_telemetry_refresh`, `request_stop`, `cancel_outbound_calls`, `start`, `stop` | `Telemetry::refresh`; `Node::start`, `stop`, `cancel_outbound_calls` (workers blocked in a call fail before they are joined), on the node, which the host holds | **closed** |
| `node.keys()` (1, the storage key) | `Identity::derive_key` under Macha's label | **closed** |
| `node.reconfigure_local` | Macha's config | **Macha's**, stays |
| `activity.note/take_bytes/idle_for` (15) | `Context::activity()`; the node notes each request's class | **closed** |
| `events.count/notify/total` (27), waits with a predicate | `Context::events()`; `kind(name)` for storage, metadata, claims; `wait_for_change` | **closed** |
| `observations().record/add/counter/event/histogram` (54) | `Context::observations()` | **closed** |
| `registry.publish_*/withdraw_*/fuse/torrent` (21) | `supervise::Registry` by name and type, the hosted system's own and not on the Context; Macha's typed accessors become one-line wrappers | **closed** |
| `SubsystemSupervisor`, `SubsystemContext`, plugin entry | `supervise::Supervisor`, `SubsystemContext.host`, `PluginEntry` with the hosted system's symbol and build identity, the hosted system's own and not on the Context | **closed** |
| `supervised_thread_statuses`, `run_supervised_*` | `Context::threads()`, for the loops' and the hosted system's threads | **closed** |
| `startup_progress`, `StartupStallGate` | `supervise::StartupProgress`, `StartupStallGate`, `StartupWatch`, which the hosted system starts | **closed** |
| `placement::capacity_placement_nodes` (2), `order_commit_replicas` (2), `placement_logical_capacity` (1) | `Nodes::placement`, `placement::*` functions; capacity comes from a named advert | **closed** |
| `RpcLinks` (tests' fault injection: connect, send, admit; `cluster/net.hpp:398`) | `NodeConfig::dial_hook`, `ClientOptions::dial_hook`: a refusal before a dial | **open** for send and admit: connect is closed. By Tom's ruling those two seams are built only if the port's case mapping finds a Macha-visible behaviour that needs them, so this waits on that mapping |
| `NodeInfo` fields the store fills (capacity, used, generation, write floor) | `NodeRecord` has adverts in place of them (`Nodes::advertise`) | **closed** |

Two things the table's own verdicts do not show, from the same read:
`wait_guard()` is on the Context as well (`context.h:341`), and a Macha worker
that must not wait past a deadline uses `call_and_wait`, whose deadline, like
`Routes::call`'s and `Time::after`'s, is a real timer however time is injected
(inventory, "Injected time").

Everything storage-shaped (`DataResourceArbiter`, `RetainedMemoryLedger`
uses by the store, `LocalState`, metadata, heads, retention) is Dagda's
or Macha's and is not in this table.

Macha's move to Dannan's wire is a flag day: no protocol 23 mode.
