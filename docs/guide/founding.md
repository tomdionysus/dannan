# Founding a cluster, and agreement

## Founding

A cluster begins as nodes that share a key (or a CA) and know one seed. They find each other by
gossip; there is no leader to elect and no step that makes one node special.

What does need a beginning is **agreement**. Agreement (dannan::agree::Agreement, Context::agreement())
is leaderless consensus per key: each **space** a hosted system serves is divided into a fixed number
of **slots**, and each slot is served by a **replica set** of nodes whose acceptors vote on every
change to the names in it.

The cluster's slot table is fixed for its life and the same on every node: the slot count
(NodeConfig::agreement_slots) and the **genesis set** (NodeConfig::agreement_genesis), the nodes that
serve every slot before anything has been recorded. **Founding** a space records every slot as served
by the genesis set, once, so the genesis constant is used once and every later change has a
recorded set to start from. One node calls `found(space, deadline, ...)`:

- it is committed when every slot is;
- with the genesis set's majority out of reach, it is answered by its one deadline (0.12.0; it once
  took a deadline per window of sixteen slots);
- it is repeatable: founding a founded space changes nothing.

A slot's replica set then moves by `transfer`, from one set to another, one round on majorities of
both, so a cluster founded on one node grows to many slot by slot.

Why a genesis set and not a founder: any node of the set may found, and a founding that half
happened is finished by founding again.

## Coming

Founding without a founder (item 4 of the stack plan, a later minor) makes founding one cluster act any
node can carry out, writing a genesis record, and refuses a double founding loudly. Until then the
genesis set is configuration, and Lugh's and Mimir's own init commands found their spaces.

## Where to read on

- dannan::agree::Agreement: found, change, read, transfer.
- dannan::NodeConfig::agreement_slots and dannan::NodeConfig::agreement_genesis.
- TODO/2026-11-15-agreement-design.md: the design.

## The example

examples/founding_three.cpp, built and run with the tests:

\include founding_three.cpp
