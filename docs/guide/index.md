# Dannan

Dannan is the network layer of the Cluster Stack: the virtual PHY and MAC a set of nodes run on,
on any mix of home hardware and rented cloud and behind any NAT. It carries frames between nodes
and knows who the nodes are; it stores nothing. A hosted system (Dagda, the store; Lugh, Mimir,
Kvasir, Louhi, Lir, Macha) runs on a node through its Context.

This reference documents every public declaration of the headers held to the documentation gate:
from 0.12.0 a release is tagged only once everything it adds or changes is documented here, and
the rest of the public API joins as the backfill reaches it (TODO/ACTIVE.md, "Documentation").

## The model

Read these first: each says what Dannan does and why.

- [Nodes and identity](nodes.md): what a node is, how it proves who it is, reachable before ready.
- [Membership](membership.md): the roster, gossip, the agreed roster, partitions.
- [Founding and agreement](founding.md): slots, the genesis set, founding a space.
- [The clocks](clocks.md): steady time, wall time, and the hybrid logical clock's rules.
- [Calls and channels](delivery.md): message types, and the four delivery kinds.
- [Deadlines, faults and evidence](faults.md): the shape of every asynchronous answer.
- [Settings and running a node](settings.md): the reader's layers, cluster.*, and the helper.

## Where to start in the reference

- A hosted system's view of its node: dannan::Context.
- A node and its configuration: dannan::Node and dannan::NodeConfig.
- A node binary's main: dannan::supervise::run_node_program, in supervise/node_program.h.
- A node's readiness and health: dannan::status::Readiness.
- The clocks every component is given: dannan::contract::TimeSource and dannan::contract::WallClock.
- Running programs: dannan::supervise::ProcessCommands.
- Limits on callers: dannan::listen::RateLimiter and dannan::listen::Backoff.
- Stored passwords: dannan::identity::make_password.
- Large transfer buffers: dannan::sched::BufferPool.

The concept guides (the model: nodes, membership, founding, the clocks, delivery, settings, faults
and evidence) and the worked examples under examples/ join this reference as they are written.
