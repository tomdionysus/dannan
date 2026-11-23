# Nodes and identity

A Dannan cluster is a set of **nodes**: processes, one per machine as a rule, each with a state
directory of its own. A node is a dannan::Node; the system it hosts (Dagda, Lugh, Mimir and the
rest) sees it only through its dannan::Context.

## What a node is

A node has an **id**, sixteen random bytes made the first time it starts and kept in its state
directory (`node-id`), so it is the same node across restarts. It also has a **durability epoch**,
made fresh by every process. Peers compare the two: the same id with a new epoch is a restart, not
a reconnect, and a peer that held state for the old process knows to treat it as gone.

The state directory holds everything the node must keep: the lock that stops two processes using
it at once, the id, the roster, telemetry, the inbound answer, and, with key custody, the sealed
keys and counters. Two nodes never share one.

## How nodes prove who they are

Version 1 of the wire uses a **cluster key**, a secret every node of the cluster holds
(`cluster.key`). A node that knows it is a member.

Version 2 uses **certificates**: the cluster's CA (`dannan-ca`) issues each node a certificate over
its own key, naming its id. The node's private key never leaves its **key custody**
(`cluster.custody.*`), sealed under a systemd credential or a passphrase. A revoked certificate is
refused everywhere once the revocation has spread. A cluster can move from the key to
certificates while running, with both accepted meanwhile (`use_cluster_key`).

Why both: the cluster key is one file to copy and nothing to run, right for a small cluster of
machines one person owns. Certificates let one node be removed without changing every other node's
secret.

## Reachable before ready

A node starts in an order that matters. Its control plane comes up first, so peers can reach it and
tell it apart from a node that is down. Then whatever it recovers (its planes, declared by the hosted
system) is marked ready, one by one. Only then is it ready. dannan::status::Readiness holds this, and
after startup it holds the node's **health** too: ready, degraded, draining or not ready, the worst
of its checks. A node's record advertises its health, and placement passes over nodes that are
draining or not ready.

## Where to read on

- dannan::NodeConfig: every setting a node is built with.
- dannan::Context: what a hosted system sees.
- dannan::status::Readiness: startup and health.
- dannan::supervise::run_node_program: a node binary's main, the usual way to run one.
