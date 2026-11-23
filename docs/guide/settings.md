# Settings and running a node

## One reader, five layers

Every node binary of the stack reads its settings the same way (dannan::config::Reader), each layer
overriding the one before:

1. the binary's defaults;
2. a YAML file, named by `--config` (or `-c`), by a variable (`LUGH_CONFIG`), or a default path;
3. `secrets.yaml` beside it, so secrets live in a file of their own;
4. the environment;
5. the command line.

A setting's YAML key is the source of truth, and its flag and variable are made from it:
`cluster.listen.port` is `--cluster-listen-port` and `LUGH_CLUSTER_LISTEN_PORT`. A value that
cannot be used is refused in the same words whichever layer gave it, naming that layer.

## The node settings

Every node binary takes the same node settings, all under `cluster.*`
(dannan::config::add_node_settings): the key, the seeds, the state directory, where to listen, the
heartbeat, level 0's settings, custody and certificates. No hosted system uses `cluster.*` for a
listener of its own, so the two never meet.

## A node binary's main

dannan::supervise::run_node_program is a node binary's main: one reader for the node settings, the
binary's own and the shared `log.*` and `node.*`, then io threads, the node, the binary's hosted root,
and the signals:

- the first SIGTERM or SIGINT **drains**: the node reads draining (placement stops choosing it), the
  program's drain steps run within `node.drain_ms`, then it stops and exits 0;
- a second stops at once, exit 1;
- SIGHUP reloads (certificates, say) and never stops.

Its exit codes are the stack's: 0, 1 for a failure to start, 2 for a setting that cannot be used.

Why one helper: every binary in the stack had written the same main, each slightly differently.
Now a binary's main is the helper and its own root, and an operator runs every binary the same way.

## Where to read on

- dannan::config::Reader and dannan::config::Settings.
- dannan::config::add_node_settings and dannan::config::node_config.
- dannan::supervise::run_node_program and dannan::supervise::ProgramSpec.
- docs/config.md: the naming rule and the shared keys.

## The examples

examples/settings.cpp, and examples/hosted_node.cpp: a hosted system on its Context as a node binary.

\include settings.cpp

\include hosted_node.cpp
