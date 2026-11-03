# Macha on dannan::Context: the audit

Tom, 2026-11-03: Macha is the first customer; the design is not complete
until Macha can be refactored to use it. This lists every call Macha makes
into its network layer from outside it (`src/` minus `cluster/net.*`,
`cluster/cluster.*`, `membership`, `placement`, `telemetry`,
`public_connectivity`, `activity_clocks`, `node_events`, `node_identity`,
`observation`, `supervised`, `subsystem/`), counted by a script over the
tree at `6252dba`, and what each maps to in the Context. "Gap" rows are
closed in this step; "wire v1" rows wait for the approved wire.

| Macha call (count) | Context today | Verdict |
|---|---|---|
| `node.config()` (102) | Macha's own config | Macha's, stays |
| `node.node_id()` (65) | `Identity::id` | ok |
| `node.membership().active()` (26), `.all()` (5) | `Nodes::active`, `roster` | ok |
| `membership().snapshot()` (2) | none | gap: `Nodes::snapshot` (all and active in one read) |
| `membership().all_known_reachable()`, `directly_reachable()` | `directly_reachable` only | gap: `all_known_reachable` |
| `node.call(node or endpoint, type, payload[, frame])` (16), blocking, stall notice and no-progress deadline | async `Routes::call` with a deadline | gap: `Routes::call_and_wait` (waits for the deadline at most; never on an io thread) |
| `node.call_async` (3) and `AsyncRpc`: `get`, `wait_for`, `cancel` (6), `abort` (2), `promote` (1), `idle_for` (4) | `call` returns nothing | gap: `Routes::call` returns a `Call` handle with cancel, abort, promote, idle_for |
| `RpcReply.peer` | `CallOutcome` has the reply only | gap: `CallOutcome::peer` |
| `routes_.bind/unbind` at runtime (storage server, metadata server, accounts, torrent and ingest plugins, availability; unbind waits for calls in flight) | `Routes::add` before start only | gap: `Routes::bind(type, handler)` and `unbind(type)` at any time; `add` keeps registering the type's wire rules before start |
| `MessageRoutes::dispatch` answers 501 for an unbound type | `Handled::unrecognised` | ok |
| `node.broadcast_notify` (4), `broadcast` (metadata notice), `broadcast_best_effort` (telemetry) | `Channels::notify` only | gap: all three on `Channels` |
| `node.telemetry().views/all/persisted/persist` (6), `peer_viewers_active` | `Node::telemetry()` only, not the Context | gap: a `Telemetry` noun |
| `node.readiness()` (3), `wait_local_state_ready`, `RecoveryProgress` planes | `Node::readiness()` only | gap: `Readiness&` on the Context; `started_unix_ms` |
| `node.apply_identity_reset` (3), `propagate_identity_reset` (2), `identity_resets()` (2) | `Node` only | gap: on `Nodes` |
| `node.advertise_storage/backends/cache/metadata_generation`, `publish_self`, `announce_metadata_generation`, `remote_metadata_generation/epoch` | none | wire v1: the record's advert map (`Nodes::advertise(key, bytes)`); the generation announcements are a Macha notification type over `Channels::broadcast` plus `events` |
| `node.rpc_stats`, `rpc_server_work_stats`, `public_connectivity_status`, `refresh_public_connectivity`, `peer_latencies`, `inbound_resolution`, `traffic_totals` (Status API) | `Node` getters only | gap: `Context::diagnostics()` snapshot, `Nodes::latencies` |
| `node.signal_telemetry_refresh`, `request_stop`, `cancel_outbound_calls`, `start`, `stop` | `Node` | ok; gap: `Node::cancel_outbound_calls` (workers blocked in a call fail before they are joined) |
| `node.keys()` (storage key) | `Identity::derive_key` | ok |
| `node.reconfigure_local` | Macha's config | Macha's, stays |
| `activity.note/take_bytes/idle_for` (14) | `status::ActivityClocks` exists, not on the Context | gap: `Context::activity()`; the server notes each request's class |
| `events.count/notify/total` (26), waits with a predicate | `NodeEvents` on the Context | ok (`kind(name)` for storage, metadata, claims) |
| `observations().record/add/counter/event/histogram` (54) | injected registry, not on the Context | gap: `Context::observations()` |
| `registry.publish_*/withdraw_*/fuse/torrent` (21) | `supervise::Registry` by name and type | ok (Macha's typed accessors become one-line wrappers) |
| `SubsystemSupervisor`, `SubsystemContext`, plugin entry | `supervise::Supervisor`, `SubsystemContext.host`, `PluginEntry` with the hosted system's symbol and build identity | ok |
| `supervised_thread_statuses`, `run_supervised_*` | `SupervisedThreads` on the node | gap: `Context::threads()` for the loops' and the hosted system's threads |
| `startup_progress`, `StartupStallGate` | `supervise::StartupProgress`, `StartupStallGate` | ok |
| `placement::capacity_placement_nodes` (2), `order_commit_replicas` (2), `placement_logical_capacity` (1) | `Nodes::placement`, `placement::*` functions | ok; capacity comes from the record (wire v1: from a named advert) |
| `RpcLinks` (tests' fault injection: connect, send, admit) | none | gap: `ClientOptions::dial_hook`, a refusal before a dial, for tests |
| `NodeInfo` fields the store fills (capacity, used, generation, write floor) | `NodeRecord` as protocol 23 | wire v1: adverts |

Everything storage-shaped (`DataResourceArbiter`, `RetainedMemoryLedger`
uses by the store, `LocalState`, metadata, heads, retention) is Dagda's
or Macha's and is not in this table.

Macha's move to Dannan's wire is a flag day: no protocol 23 mode.
