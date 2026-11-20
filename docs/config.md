# Configuring a node binary

Every node binary in the stack (dannan-node, lir-node, lugh, louhi, mimir-node, kvasir-node, macha
and the rest) reads its settings with one reader, `dannan::config` (`dannan/config/config.h`), in
one pattern. The maintainer, 2026-11-20: YAML is the source of truth, and flags and environment
variables may override any key.

## The layers

Each layer overrides the one before:

1. the binary's defaults;
2. the YAML file: `--config <path>`, or `$<PREFIX>_CONFIG`;
3. `secrets.yaml` beside that file, in the same shape, for what should not sit in the main file;
4. the environment: `<PREFIX>_<KEY>`;
5. the command line: `--<key>`.

The reader records which layer gave each value. A value that cannot be used is refused in the
same words whichever layer gave it, naming the key and the source: the file's path, the
variable or the flag. A key in a file that no setting reads is reported, since it is most likely
a misspelling.

## Names

A setting is named once, by its YAML key. The flag and the variable are made from that key the
same way in every binary:

| | Rule | `log.level` | `listen.port` |
|---|---|---|---|
| YAML | dotted key; each dot is one level of nesting | `log: {level: info}` | `listen: {port: 7437}` |
| flag | `--`, then the key with `.` and `_` as `-` | `--log-level` | `--listen-port` |
| variable | the project's prefix, `_`, then the key in upper case with `.` and `-` as `_` | `KVASIR_LOG_LEVEL` | `DANNAN_LISTEN_PORT` |

The prefix is the project's name in upper case, not the binary's: lugh-node's variables are
`LUGH_...`, mimir-node's `MIMIR_...` and dannan-node's `DANNAN_...`, which is how every deployed
unit already works. A list is a YAML list, a repeated flag, or a variable split on commas and whitespace. A
toggle is `true` or `false`; the flag alone means `true`.

A binary may leave a setting off the command line or out of the environment, such as a secret
that should not appear in `ps` output. It never gives one under a name other than these.

A list whose items may hold a comma or whitespace is not offered as a variable: a variable is
split on both, and there is no escape. Kvasir's argon2id credentials are an example
(`m=..,t=..,p=..`). Such a list comes from YAML, and a secret one from `secrets.yaml`; a repeated
flag, where the binary offers one, carries each item whole.

## Shared settings

These keys mean the same in every binary:

| Key | Values | Parsed by |
|---|---|---|
| `log.level` | `debug`, `info`, `warn`, `error` (and `all`), any case | `dannan::status::parse_log_level` |
| `log.format` | `text` or `json`, any case | `dannan::status::parse_log_format` |

A setting that two binaries both need is added here, so it is named once for the stack.

## dannan-node today

dannan-node does not follow the rule yet, in two ways:
- it has its own short flags for some keys, such as `--port` for `listen.port`;
- its variables carry the binary's prefix, `DANNAN_NODE_...`, where the rule gives `DANNAN_...`.

Both are part of its interface, so both move to the rule's names in 0.11.0, the next minor.
