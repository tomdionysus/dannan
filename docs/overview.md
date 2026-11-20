# Dannan: overview

What Dannan is for and what it does. The [README](../README.md) covers installing and using it.

## Why it exists

Every distributed system that runs across machines a person actually owns
meets the same problems before it can do anything useful:

- **Reaching each other.** Home machines sit behind NATs, some behind two
  (carrier-grade NAT), and accept no inbound connection. Cloud machines are
  reachable but cost money. A cluster has to work over whatever path exists:
  direct, a hole punched through both NATs, or a relay through a node that
  can be reached.
- **Knowing who is in the cluster.** Which nodes exist, which are alive,
  what each can do, and whether a node that went away has really gone or
  just restarted.
- **Trusting each other.** Every node proves it belongs to the cluster, and
  a node that is removed or compromised is cut off at once.
- **Not starving the important work.** A control message must never queue
  behind a bulk transfer, and someone waiting on a read comes before a
  background repair.

Each system used to solve these on its own: Macha (a media filesystem)
grew its own transport, membership and cluster key, and AthenaSIP (a SIP
server) leaned on a central Redis and a central MQTT broker, both single
points of failure. Dannan solves them once, so the systems above it only
do what they are for.

What goes into Dannan is decided by one test: a thing belongs here only if
every hosted system needs it, and needs it in the same shape. Anything one
system needs in its own shape stays in that system.

## What it does

- **Reachability ("level 0").** Each node is classified as public,
  punchable or relay-only from what its peers observe. Pairs connect
  directly where they can, by a brokered hole punch where both sit behind
  endpoint-independent NATs, and through a relay otherwise. Paths move up a
  class when a better one appears. This is proven through real NATs by a
  Docker test bed.
- **Two lanes.** Every pair of nodes talks over a CONTROL lane and a DATA
  lane, so a bulk transfer never holds up a control message.
- **Routes.** A hosted system registers its own message types (numbers 512
  and up; Dannan's core uses 1 to 511) and calls any node with a deadline.
  A call returns the reply or why there was none, never a bare boolean. A
  call whose deadline passes is cancelled.
- **Channels.** Named channels with a delivery contract chosen when opened:
  notify (at most once), stream (ordered, replayable) or bulk
  (acknowledged). Subscriptions are by topic prefix and gossiped, so a
  publish goes only where someone listens.
- **Membership.** The roster of nodes, gossiped and persisted, with each
  node's capabilities, path class, incarnation (to tell a restart from a
  reconnect) and adverts (small values a hosted system publishes in its
  node's record). A feed of joins, departures and changes, and how long a
  node has been absent, kept across a restart.
- **Identity.** A node proves itself by a shared cluster key (wire version
  1), by an X.509 certificate from the cluster's own CA (wire version 2),
  or both while a cluster moves from one to the other. A revocation signed
  by the CA cuts a node off at once and reaches every node within one
  gossip round. Each node's private key is held in key custody, sealed
  under a systemd credential or an Argon2id passphrase, and kept in locked
  memory.
- **Placement.** Pure functions over the roster: given a key, the ordered
  nodes that should hold it, spread across failure domains, under each
  hosted system's own salts.
- **Work classes and budgets.** Control, foreground, loader and
  speculative work are queued, admitted and paced in that order (the four
  laws in `docs/principles-and-laws.md`), with memory budgets per class
  and a weighted duty cycle for background loops.
- **Status.** Telemetry gossiped between nodes, readiness, counters,
  structured JSON logs, observations, and supervision of every thread.
- **Shared foundations.** Secure memory, signing (ES256 and Ed25519),
  X25519, Argon2id, durable files, percent-encoding, the layered settings
  reader every node binary uses, fuzz targets and test fixtures, written once here and used
  by every project above.

Dannan stores nothing on a hosted system's behalf and knows no file, key,
user or merge rule. A node can run Dannan alone, as a relay that holds the
roster and forwards traffic for others.
