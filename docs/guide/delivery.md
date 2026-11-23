# Calls, channels, and the delivery kinds

A hosted system talks to its peers in two ways.

## Calls

A **call** goes to one peer and gets one reply (dannan::Routes::call). A hosted system registers its
message types before the node starts, on every node that sends or answers them, with their rules:
which lane (CONTROL or DATA), which frame types, whether a peer's requests run one at a time. A
handler answers by returning, or takes the request's dannan::net::Answer and gives it later from any
thread, holding no worker while it waits.

Every call has a deadline. When it passes, the caller is told, the peer is told to stop, and a reply
that comes later is dropped. A call's work class (control, foreground, read-ahead, loader,
speculative) decides its queue at both ends, so background work never delays foreground work.

## Channels

A **channel** carries publishes to whoever subscribes (dannan::Channels). Subscription is by topic
prefix and local to the node; each node gossips the prefixes it has subscribers for, so a publish
goes only to the nodes that want it, and a topic nobody subscribes to costs nothing.

A channel's **delivery kind** is chosen when it is opened, and never implied:

| Kind | Lane | Guarantee | Under pressure |
|---|---|---|---|
| notify | CONTROL | at most once, no order | dropped |
| ordered | CONTROL | at most once, in order per origin; a late message is dropped | dropped |
| stream | CONTROL | at least once, in order per origin; gaps refilled from the sender's replay buffer, what it no longer holds reported as lost | held for replay |
| bulk | DATA | each destination acknowledges; the publisher is told who did | backpressured |

There is no exactly-once delivery, and no order across origins. Why: both cost a round of agreement
per message, and the systems that need them (a store's commit, say) build them on calls and
agreement, where they can afford it.

Which to choose:

- **notify** for facts that the next one replaces: a gauge, a "something changed" signal.
- **ordered** when order matters and a lost message may be lost: MQTT's ordered QoS 0 (Lir).
- **stream** when every message matters and arrives in order, within a replay buffer's reach.
- **bulk** for large payloads where the publisher must know who has them.

## Evidence

Something that happened on more than one node is never a bare yes or no. A publish returns
dannan::net::Published: who took it and who it missed, with why. A pushed advert returns who holds it.
A caller decides what is enough.

## Where to read on

- dannan::Routes, dannan::net::Answer, dannan::net::ServerLimits.
- dannan::Channels, dannan::net::Delivery, dannan::net::Fanout.

## The examples

examples/routes.cpp and examples/delivery.cpp, built and run with the tests:

\include routes.cpp

\include delivery.cpp
