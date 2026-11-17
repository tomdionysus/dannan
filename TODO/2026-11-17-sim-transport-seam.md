# The simulation's transport seam (step 3, part 4), for review

For Kvasir and Mimir, before it is built (the harness design: "Routes, roster and placement in
phase 1 ... membership, Routes and placement in the world"). The harness so far runs agreement
alone, its `Call` routed through the world. Part 4 puts whole nodes in the world: membership
(the roster, gossip, a node seen leaving by some and not others), `Routes::call`, placement over
each node's own roster, the channels, and agreement over all of them, under the seed.

## What stands in the way

A `Node` today is real time and real threads:
- **Sessions**: TCP and TLS through `net::Client` and `net::Server`, Asio sockets and timers on the
  steady clock, a handshake, the scheduler's lanes and pools.
- **Four loop threads** (`cluster-maintenance`, `-telemetry`, `-connectivity`, `-roster`), each
  `sleep_until` on `NodeConfig::time`, woken by a condition variable.
- The client's surface is wide: calls, sends, broadcasts, probes, lanes, relays, punching.

A seed cannot reproduce any of that: threads interleave as the OS likes, and sockets carry real
bytes on real time.

## Options

1. **A socket seam.** Sessions over an injected byte stream the world delivers; the handshake,
   the scheduler and TLS run for real. Most faithful. But every Asio timer and strand in
   `Client`, `Server`, `Session` and the scheduler would move onto injected time, and the
   scheduler's pools onto the world: the whole network layer rewritten for a harness, and TLS
   in the world. Not recommended.
2. **A call seam (recommended).** One interface under the node, `net::Transport`: what the node
   needs of the network, at the level of messages to peers. The real one wraps `Client` and
   `Server` as today; the world's delivers each message as an event (delayed, lost, duplicated,
   reordered, one-way blocked, as `Cluster::send` does now). With it:
   - `NodeConfig::transport`, an optional `std::shared_ptr<net::Transport>`: absent, the node
     builds its own over sockets, as now.
   - `NodeConfig::schedule`, as `Agreement::Schedule`: present, the four loops become tasks the
     schedule runs each heartbeat, on the node's executor, with no threads; absent, threads as
     now.
   - The surface, from what the node calls today: `call(peer or endpoint, type, payload, frame,
     deadline, executor, done)`, `send` and `broadcast` (best effort and notify), `note_peer`,
     `set_maintained_peers`, `close_route`, `probe`, and the inbound side (`request`,
     `established`, `alive`). Lanes and frame classes are carried as values, not sessions.
     Relay, punching and dial-back stay outside: the world's peers are reachable or not, by its
     matrix.
   - What it does not test: sessions, the handshake, TLS, the scheduler's admission and lanes.
     Those keep their own tests and the real two-process runs; the simulation is for the
     protocols above them.
3. **Agreement only, membership modelled.** Keep the world as it is and model the roster and
   placement in the harness. Cheap, but it tests a model, not Dannan's membership, which is what
   the design asked for ("never a model of them").

## Recommended: option 2, in two pieces

- **2a, the loops on a schedule** (no transport change): `NodeConfig::schedule`; each loop's
  body becomes one step, run by a thread as now or by the schedule. Testable alone: a node whose
  loops run on a stepped schedule does in one step what one wake of the thread does.
- **2b, the transport**: `net::Transport` and the world's `SimTransport`, a `SimNode` fixture
  (a `Node` on the world's transport, time and schedule), and the first runs: nodes join, gossip
  converges, one node seen leaving by some and not others, rosters that differ briefly, placement
  over each node's own roster, agreement on top, the broken protocol "two rosters naming
  different sets" from real membership instead of a rogue proposer.

## Mimir's review (2026-11-17), taken

Option 2, in 2a and 2b. Mimir needs no session layer in the world: lanes and frame classes as
values suffice. Its actors use `call()` (agreement's Call, and forwarding a command to a node in
the key's set), `send()` and `broadcast()` best effort (merge ops, PUBLISH, keyspace
notifications, tracking invalidations), each under the world's faults, and adverts (the slot
advert, its capability advert) delivered through membership in the world, so a stale advert is
real. Its RESP listener and MIGRATE's outbound connection stay outside; its actors drive
`Database::execute` directly. Three asks, taken into 2b:
1. Each node's clock at its own offset and drift, from the seed (the hybrid clock and Mimir's
   merge class are judged under skew).
2. A node stopped and restarted with its hosted store kept or wiped, as the test chooses.
3. Partitions on the node matrix as well as per message, healed at a chosen tick.

## Kvasir's review (2026-11-17), taken

Option 2 fits: Kvasir's protocols across nodes are calls (forwarding a range's commits to its
sticky proposer, row-lock calls, the transaction record, catch-up copies, agreement), and the
world's faults cover what a stalled lane would do to them. Its MySQL listener stays outside;
its actors drive sessions directly. Its ask: its own timers (lock-wait timeouts, the proposer's
batching window, catch-up retries) on the world's time. `Context::time()` already gives
`source()` and `after(delay, fn)`, but `after()` is woken by an Asio timer, which the world cannot
order. Taken into 2a: with `NodeConfig::schedule` set, `Time::after()` runs through it, so a
hosted system's timers are the world's events with nothing more on its side.

## Piece 2b: the surface, from the node as it is (2026-11-17)

What `Node` asks of `_client` and `_server` today (by call sites in `node.cpp`), and what the
world's transport gives for each:

| The node's use | Real transport | World's transport |
|---|---|---|
| `call` (a record or an endpoint), `send`, `broadcast`, `broadcast_best_effort`, `broadcast_notify` | the client, as now | events through the world: delay, loss, duplication, reorder, one-way blocks, the node matrix |
| `has_route`, `close_route`, `note_peer`, `set_maintained_peers` | the client | a route is the matrix's reachability; notes kept for `has_route` |
| `start`, `stop`, `bound_port` | client and server | a port per simulated node; stop fails calls in flight |
| the server's handler (`handle`), its `scheduler` and `work_stats` | the server, as now | requests delivered to the node's `handle()` on its executor; a scheduler for admission as configured, no lanes |
| `stats`, `peer_latency`, `peer_latencies`, `path_of`, `stalled`, `release_stalled` | the client | figures of the world (zero, or the world's delays) |
| `probe`, `handle_dial_request`, `admit`, `invalidate_identity_association`, and `reach::Paths` (relay, punching) | the client and reach | outside the world: a simulated node is reachable or not by the matrix, and its path class is set |

So `net::Transport` is the first block and the route notes; a `Transport::Figures` for the
figures; the reach layer stays with the real transport only. `CallHandle` is made by either
(today only `Client` makes one: its placement hooks become the transport's).

## Questions

1. Kvasir, Mimir: is the call seam enough for your simulations, or does either need the
   session layer in the world (a stalled lane, a frame class starved)?
2. Mimir: your client actors drive `Routes::call` and channels; anything there not in the list?
3. Both: the node's `Context` is unchanged; a hosted system's own code needs no seam of its own
   unless it opens sockets itself (Kvasir's MySQL listener does: kept outside the world?).
