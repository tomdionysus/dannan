# Dannan - The Distributed Substrate

[![Release](https://img.shields.io/github/v/tag/tomdionysus/dannan?sort=semver&label=release)](https://github.com/tomdionysus/dannan/tags)
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

[`docs/overview.md`](docs/overview.md) says why Dannan exists and what it does.

## Install

On Debian 13 (x86-64 or arm64), from the tomcully.com apt repository:

```
sudo curl -fsSLo /etc/apt/sources.list.d/tomcully.sources https://tomcully.com/apt/tomcully.sources
sudo apt update
sudo apt install libdannan-dev    # the library, its headers and CMake package
sudo apt install dannan-tools     # dannan-ca, dannan-custody, dannan-import-macha, dannan-version
```

Elsewhere, build it (below) and install it:

```
cmake --install build --prefix /opt/dannan
```

A dependant's CI can build and install a release's packages in one step:
`tools/ci.sh install-debs <tag>`. [`docs/packaging.md`](docs/packaging.md)
covers the Debian packages.

## Use

### Link

In a project that hosts on Dannan, name the oldest patch it needs. A patch
release keeps the ABI, so any later patch of the same minor is accepted and
loads:

```cmake
find_package(dannan 0.11.1 CONFIG REQUIRED)
target_link_libraries(my_system PRIVATE dannan::dannan)
```

A project's tests can link `dannan::testing`: the fuzz harness (register targets,
run them through `dannan::fuzz::smoke` on every gate, or build a fuzz binary from two
lines) and the simulation's checkers.

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
dannan-node --cluster-key cluster.key --cluster-state ./a --cluster-listen-port 7437 \
  --cluster-listen-advertise 192.0.2.10
dannan-node --cluster-key cluster.key --cluster-state ./b --cluster-listen-port 7437 \
  --cluster-listen-advertise 192.0.2.11 --cluster-bootstrap 192.0.2.10:7437 --echo-ms 2000
```

Every option can also come from a YAML file, with the flags winning over
it: `--config node.yaml`, or `$DANNAN_CONFIG`. A `secrets.yaml` beside
the file, in the same shape, wins over the file, and each option has a
`DANNAN_*` variable that wins over both. `--cluster-listen-port 7437` is
`cluster: {listen: {port: 7437}}` in the file and `DANNAN_CLUSTER_LISTEN_PORT`
in the environment. This is `dannan::config` (`dannan/config/config.h`), the
one settings reader every node binary in the stack shares, and the `cluster.*`
settings are every node binary's (`dannan/config/node_settings.h`), each under
its own project's prefix; [`docs/config.md`](docs/config.md) has its layers and
the naming rule every binary follows, and [`docs/running.md`](docs/running.md)
what bounds a node's memory.

### Run a cluster on certificates

`dannan-ca` is the cluster's certificate authority. It runs on an
operator's machine, never on a node, and its key is sealed under a
passphrase or a systemd credential.

```
export CA_PASS=... NODE_PASS=...
dannan-ca init --dir ./ca --name my-cluster --passphrase-env CA_PASS

dannan-node --cluster-state ./a --init-custody --cluster-custody-passphrase-env NODE_PASS
dannan-node --cluster-state ./a --show-identity --cluster-custody-passphrase-env NODE_PASS
#   {"id":"<node id>","public_key":"<hex>"}
dannan-ca issue --dir ./ca --passphrase-env CA_PASS \
  --node <node id> --public-key <hex> > ./a/node.pem

dannan-node --cluster-state ./a --cluster-custody-passphrase-env NODE_PASS \
  --cluster-identity-ca ./ca/ca.pem --cluster-identity-certificate ./a/node.pem \
  --cluster-identity-no-cluster-key ...
```

On a production node, `dannan-custody` makes custody under a systemd
credential, with the node stopped, and prints the new root once for
sealing; `show` proves the sealed credential opens it before the node is
started on it:

```
dannan-custody init --state /var/lib/node --credential dannan-root \
  | systemd-creds encrypt --name=dannan-root - /etc/credstore.encrypted/dannan-root
dannan-custody show --state /var/lib/node --credential dannan-root \
  --credentials-dir <where the credential is decrypted>
#   {"id":"<node id>","public_key":"<hex>"}
```

To remove a node, revoke it and hand the revocation to any node; it
reaches the rest within one gossip round:

```
dannan-ca revoke --dir ./ca --passphrase-env CA_PASS --node <node id> \
  --reason "decommissioned" --out revocation.bin
dannan-node ... --cluster-identity-revocations revocation.bin
```

## Build from source

Dannan needs a C++20 compiler, CMake 3.24 or later, Boost 1.83 or later (Asio,
headers only), OpenSSL 3 and yaml-cpp; GoogleTest for the tests.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TESTING=ON
cmake --build build -j8
./build/dannan_tests
```

Pass `-DCMAKE_PREFIX_PATH` where Boost and OpenSSL are not on the default
path (on macOS with MacPorts, for example
`"/opt/local/libexec/boost/1.88;/opt/local"`). [`docs/building.md`](docs/building.md)
has every option, including `DANNAN_SANITIZE` for an instrumented build.

## The stack

Dannan is the bottom of a stack of projects, each its own library and
repository, written to the same conventions, with no code duplicated between
them: a piece needed by more than one lives in exactly one.

- [Dannan](https://github.com/tomdionysus/dannan): the network (reachability,
  routes, channels, membership, identity, agreement, placement, work classes,
  status).
- [Dagda](https://github.com/tomdionysus/dagda): the store (data domains,
  sealing, objects, heads, leases), a hosted system on Dannan.
- [Lugh](https://github.com/tomdionysus/lugh): the directory and sign-on.
- [Lir](https://github.com/tomdionysus/lir): a distributed, multimaster MQTT
  broker.
- [Louhi](https://github.com/tomdionysus/louhi): distributed compute.
- [Mimir](https://github.com/tomdionysus/mimir): a distributed key-value store
  speaking the Redis protocol.
- [Kvasir](https://github.com/tomdionysus/kvasir): a distributed SQL database
  speaking the MySQL protocol.
- [Macha](https://github.com/tomdionysus/macha): a media filesystem.
- [AthenaSIP](https://github.com/blackravenltd/athenasip): a SIP server.

Hosted systems never address one another. They exchange frames through
Dannan and, where they share state, share it through Dagda.

## Status

Version 0.11.1. The library, wire versions 1 and 2, reachability, identity and
agreement are complete and tested. Within a minor the ABI is kept; interfaces
may change between minors while the hosted systems are built on them.

`CONTRIBUTING.md` has the conventions, the layout and how to run the test
suites, the fuzz targets and the Docker test bed. Everyone taking part follows
the [Code of Conduct](CODE_OF_CONDUCT.md).

## Licence

Copyright (C) 2026 Tom Cully. Licensed under the GNU GPLv3; see `LICENSE`.
