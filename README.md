# Dannan - The Distributed Substrate

[![CI](https://github.com/tomdionysus/dannan/actions/workflows/ci.yml/badge.svg)](https://github.com/tomdionysus/dannan/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/tomdionysus/dannan/graph/badge.svg)](https://codecov.io/gh/tomdionysus/dannan)
[![CodeQL](https://github.com/tomdionysus/dannan/actions/workflows/codeql.yml/badge.svg)](https://github.com/tomdionysus/dannan/actions/workflows/codeql.yml)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

Dannan is the network layer for a cluster of trusted machines. It is a C++20
library that makes a set of nodes, on any mix of home hardware and rented
cloud and behind any NAT, behave as one network that the systems hosted on
them can transmit over. In networking terms it is the virtual PHY and MAC:
it carries frames between nodes and knows who the nodes are, and it stores
nothing.

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

## The stack

Dannan is the bottom of a stack of projects. Each is its own library and
repository, written to the same conventions, and no code is duplicated
between them: a piece needed by more than one lives in exactly one.

```
   Lugh            Lir             Macha, AthenaSIP, ...      hosted systems
     |               |                  |
   Dagda: the store  (domains, sealing, objects, heads, leases)
     |
   Dannan: the network  (reachability, routes, channels, membership,
                         identity, placement, work classes, status)
```

- **Dagda** is the store: data domains and their keys, sealing at rest,
  immutable objects, heads (a commit DAG whose merge the owner supplies),
  leases and claims, per-origin counters and node-local spaces. It is a
  hosted system on Dannan and uses Dannan for all of its networking.
- **Lugh** is the directory and sign-on: people and services as
  principals, groups, grants, two-factor sign-in, OAuth 2 and OpenID
  Connect, and read-only LDAP. It runs on Dannan, keeps everything it
  persists in Dagda, and is the source of every principal's scope, which
  it installs into Dannan's scope check.
- **Lir** is the event system: a distributed, multimaster MQTT broker
  (MQTT 5.0 and 3.1.1, QoS 0, 1 and 2, retained messages, wills, shared
  subscriptions, persistent sessions). It runs on Dannan and persists in
  Dagda. Its authentication is pluggable; a Lugh plugin is one option.

Hosted systems never address one another. They exchange frames through
Dannan and, where they share state, share it through Dagda.

## How to use it

### Build

Dannan needs a C++20 compiler, CMake 3.24 or later, Boost 1.88 (Asio),
OpenSSL 3 and yaml-cpp; GoogleTest for the tests.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TESTING=ON
cmake --build build -j8
./build/dannan_tests
```

Pass `-DCMAKE_PREFIX_PATH` where Boost and OpenSSL are not on the default
path (on macOS with MacPorts:
`"/opt/local/libexec/boost/1.88;/opt/local"`).

### Install and link

```
cmake --install build --prefix /opt/dannan
```

Then, in a project that hosts on Dannan:

```cmake
find_package(dannan 0.4 CONFIG REQUIRED)
target_link_libraries(my_system PRIVATE dannan::dannan)
```

### Host a system on a node

A hosted system is given a `dannan::Context`, which is the only way it
touches the network. A `dannan::Node` is one: configure it, register your
message types before it starts, start it, and call peers.

```cpp
#include <boost/asio/io_context.hpp>
#include <cstdio>
#include <thread>

#include "dannan/node.h"

using namespace std::chrono_literals;

constexpr dannan::net::MessageType greet{512};  // hosted types start at 512

int main() {
  boost::asio::io_context io;
  auto work = boost::asio::make_work_guard(io);
  std::thread runner([&] { io.run(); });

  dannan::NodeConfig config;
  config.state_path = "/var/lib/my-system/node";  // roster, node id, state lock
  config.key = dannan::identity::load_cluster_key("/etc/my-system/cluster.key");
  config.advertise_host = "192.0.2.10";
  config.bootstrap = {{"192.0.2.11", 7437}};  // seeds, not masters

  dannan::Node node(io, config);
  auto& context = node.context();

  // Answer greet on the CONTROL lane with the request's own payload.
  context.routes().add(greet, {"greet", dannan::net::Lanes::control, dannan::net::Frames::any, false},
                       [](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
                         return dannan::net::Handled::processed({dannan::net::message::ok, request.payload, {}});
                       });
  node.start();

  // Call every active peer; each answer is posted to the executor given.
  for (const auto& peer : context.nodes().active()) {
    if (peer.id == context.identity().id()) continue;
    context.routes().call(peer, greet, {'h', 'i'}, dannan::sched::FrameType::control, 2s, context.time().executor(),
                          [](dannan::net::CallOutcome outcome) {
                            if (!outcome.reply) std::fprintf(stderr, "no reply: %s\n", outcome.failure.c_str());
                          });
  }

  // ... run until asked to stop ...
  node.stop();
  work.reset();
  runner.join();
}
```

The rest of the Context is in `dannan/context.h`, written as a
specification: every operation states what it waits on and its thread
safety, every completion is posted to the executor the caller passed, and
no call waits without a deadline. Its nouns are `nodes()` (the roster,
adverts, placement), `routes()`, `channels()`, `identity()`, `budget()`,
`faults()`, `time()`, `telemetry()`, `status()`, `events()`,
`readiness()` and `threads()`.

### Run a node

`dannan-node` is a node with nothing above it but an echo. It is useful for
trying a cluster out and is what the test beds run. It reports what it sees
as one JSON object per line.

```
printf '%s' 'a long shared secret' > cluster.key
dannan-node --key cluster.key --state ./a --port 7437 --advertise 192.0.2.10
dannan-node --key cluster.key --state ./b --port 7437 --advertise 192.0.2.11 \
  --bootstrap 192.0.2.10:7437 --echo-ms 2000
```

Every option can also come from a YAML file, with the flags winning over
it: `--config node.yaml`, or `$DANNAN_NODE_CONFIG`. A `secrets.yaml` beside
the file, in the same shape, wins over the file, and each option has a
`DANNAN_NODE_*` variable that wins over both. `--port 7437` is
`listen: {port: 7437}` in the file and `DANNAN_NODE_LISTEN_PORT` in the
environment. This is `dannan::config` (`dannan/config/config.h`), the one
settings reader every node binary in the stack shares.

### Run a cluster on certificates

`dannan-ca` is the cluster's certificate authority. It runs on an
operator's machine, never on a node, and its key is sealed under a
passphrase or a systemd credential.

```
export CA_PASS=... NODE_PASS=...
dannan-ca init --dir ./ca --name my-cluster --passphrase-env CA_PASS

dannan-node --state ./a --init-custody --custody-passphrase-env NODE_PASS
dannan-node --state ./a --show-identity --custody-passphrase-env NODE_PASS
#   {"id":"<node id>","public_key":"<hex>"}
dannan-ca issue --dir ./ca --passphrase-env CA_PASS \
  --node <node id> --public-key <hex> > ./a/node.pem

dannan-node --state ./a --custody-passphrase-env NODE_PASS \
  --ca ./ca/ca.pem --certificate ./a/node.pem --no-cluster-key ...
```

To remove a node, revoke it and hand the revocation to any node; it
reaches the rest within one gossip round:

```
dannan-ca revoke --dir ./ca --passphrase-env CA_PASS --node <node id> \
  --reason "decommissioned" --out revocation.bin
dannan-node ... --revocations revocation.bin
```

## Status

Version 0.5.3. The library, wire versions 1 and 2, reachability and
identity are complete and tested. The interfaces may still change between
minor versions while the hosted systems are built on them. Macha and
AthenaSIP will be ported onto Dannan and Dagda later.

`CONTRIBUTING.md` has the conventions, the layout and how to run the test
suites, the fuzz targets and the Docker test bed.

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
