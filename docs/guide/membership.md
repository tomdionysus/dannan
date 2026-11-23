# Membership, the agreed roster, and partitions

## The roster

Every node keeps a **roster**: a record for each node it knows of, itself first. A record says where
the node is (host, port, its candidates for level 0 paths), its failure domain, its capabilities
(inbound, relay), its incarnation, and its **adverts**: small values a hosted system sets on its own
node (its capacity, the slots it serves, its health) and Dannan carries without reading.

Rosters spread by **gossip**. Every heartbeat (five seconds by default) a node exchanges its roster
with its seeds and the peers it knows; each merges what it is sent, the newer record winning. A
change to a node's own record (a new advert, say) can also be pushed straight to every peer
(dannan::Nodes::advertise with a deadline), for a hosted system that must know its peers hold it.

A node is **active** while it has been heard of within `dead_after` (thirty seconds), directly or by
gossip. One not heard of for that long leaves the active roster but is still known; one not heard of
for `forget_after` (thirty days) is forgotten.

Why gossip and no master: there is nothing to elect, nothing to fail over, and a node behind a NAT
that can reach one peer reaches them all.

## The agreed roster

Absence is not leaving. A node that is down for an hour is still a member: the cluster must still
count it when it asks whether it has a majority. So beside the roster each node keeps the **agreed
roster**: every node seen to join, which only an operator's **retire** takes out
(dannan::Nodes::retire), never silence.

## Partitions

When the network splits, each side must know whether it is the majority. dannan::Nodes::partition
reads the agreed roster: a side is a majority only with more than half of it reachable, this node
counted. An even split is a minority on both sides, so neither acts as the whole cluster. Every
hosted system reads the same rule, so they never disagree about which side they are on.

Why the agreed roster and not the active one: if absence shrank the count, two halves of a split
could each count only themselves and both call themselves a majority.

## Where to read on

- dannan::Nodes: the roster, adverts, the partition reading, retire.
- dannan::RosterChange and dannan::AdvertChange: watching the roster and one advert.
