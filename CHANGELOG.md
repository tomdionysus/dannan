# Current release

## 0.5.6: Debian packages

Packaging: Dannan is packaged for Debian trixie on amd64 and arm64, for the
tomcully.com apt repository. `libdannan<version>` (for example `libdannan0.5.6`)
is the shared library, `libdannan-dev` the headers, the `libdannan.so` link and
the CMake package configuration, depending on exactly its own release's library,
and `dannan-tools` carries `dannan-ca`, `dannan-import-macha` and `dannan-version`,
each with a manual page. Debug symbols come as `-dbgsym` packages.

Compatibility: the SONAME is now the whole version, `libdannan.so.0.5.6`, where it
was the major and minor (`libdannan.so.0.5`). No release promises another's ABI
yet, so a host built against one release loads only that release, and two releases
can be installed side by side. The CMake package matches: `find_package(dannan
0.5.6 CONFIG)` accepts only that exact version, where any 0.5 was accepted before.
The library's code is unchanged from 0.5.5.

## 0.5.5: a stop cannot fall between a dial and its slot

Transport: `Client::dialled` read whether the client had stopped and took the
dialled session's slot in two separate sections, so a stop between them could leave
a route open. Both now happen in one section.

Level 0: a refused simultaneous open was retried every 0.2 to 2 ms, which hammered
a closed port with hundreds of attempts a second. The retry is now close at first,
widening to a 5 ms mean. Retry timing alone still did not let two ends meet reliably
on some Linux loopbacks, so a refused end now also listens on its punch port until
the open ends, taking only the far end's punch endpoint and what its listener has
queued.

## 0.5.4: a call on a session the peer retired ends at stop

Transport: a session the peer retired was dropped from the route tables untracked,
so `Client::stop()` missed a call waiting on it, which then ran to its deadline.
Every session made a route is now held until stop closes it. A refused simultaneous
open retried at a fixed 5 ms, so two ends that began out of step stayed out of step;
it now retries after a random interval.

Telemetry: `Telemetry::set_bytes` with an empty value stored zero bytes; it now
removes the field, as the header says and every other setter does for its default.

Build: the tree builds clean under g++ with warnings as errors, by renames only,
with GCC's libstdc++ false positives ignored around the codec's writer alone.

## 0.5.3: the installed package finds Debian's Boost

Build: the installed CMake package asked for Boost 1.87 while the build accepts
1.83, so nothing could `find_package(dannan)` on Debian. One number now feeds both.
CMake only; the library's code is 0.5.2's.

## 0.5.2: a retiring session cannot hold stop

Transport: when two sessions to one peer settled into one, the loser left the route
tables under the client's lock and joined the retiring list only after the lock was
released. A `Client::stop()` between the two found it in neither, so a call already
on it waited out its deadline. A loser that reaches retirement after stop is now
closed, an admission holds an operation across its table change that stop waits
for, and a client operation ends through a guard, so a throw on the way out cannot
hold stop for ever.

## 0.5.1: relay chunks numbered and credited

Level 0: a relay circuit's chunks are numbered, so a hop may pipeline them and they
may move between sessions, and the far end puts them back in order. Circuits are
paced by credit, every wait for a missing chunk is bounded, and a circuit finishes
only after its last chunk; the exact count comes only after an orderly drain (wire
v1 section 6). A relay closing a circuit for a lost end is counted.

Transport: a session runs a copy of its handler, so a reply that closes the session
cannot destroy the one running. Bytes sent count as a call's progress, a peer is
known before its first request is read, and nothing is delivered after a session
closes. The listener keeps going through a failed admit or accept. Node: the node's
loops are woken under their wait's mutex; a forced public connectivity refresh asked
for between a loop's test and its wait had waited for the next heartbeat, and
telemetry demand and the roster write were delayed the same way. The roster is
written off the io threads.

## 0.5.0: Macha's behaviour, case by case

Dannan matched against Macha's own behaviour and cases, for the port. The journal's
lines are written in Macha's words through the node's sink, with stall notices and
per-type deadlines; health probes behave as Macha's do, one pass at a time, and a
lost control route is dialled again. The lifecycle follows Macha's: readiness,
closed sessions, escalation, supervised threads and `request_stop`. A call is
watched for progress only when it asks. A call's deadline, the node's timers, the
health loop and the supervisor's backoff follow injected time. An empty advertised
host and failure domain fall back as Macha's do, `frame_type_name` says
`read-ahead`, and a UPnP removal that cannot find the gateway keeps the mapping to
try again. New tools: `dannan-import-macha` converts Macha's state files to Dannan's,
writing the `dagda/extents` advert, and `dannan-bench` measures the transport,
through the test bed's NATs among other paths.

Fixes: the handshake's evaluation order crashed every inbound connection when built
with g++; Dannan now builds on Debian 13 with Boost 1.83. A fault sink kept past a
supervisor's stop was a use-after-free and is now ignored, and the supervisor clears
at stop. Stopping could destroy a condition variable still in use. Each log record
is written whole, so two nodes in one process do not interleave on stderr. Level 0:
a relay circuit is paced end to end and waits out a full queue instead of closing,
a DATA circuit's bytes take the DATA lane at each hop (they had all gone on CONTROL,
ahead of its pings), a relayed session counts its circuit's movement as proof of
life and gets the whole liveness budget again after its path moves, and a punch is
retried almost at once after a refusal.

## 0.4.1: hosted systems declare their readiness planes

Status: `status::Readiness::declare`, through the Context, lets a hosted system
declare its own readiness planes until the node starts, so the node is never ready
before every plane is known. After start it throws `std::logic_error`. A patch, so
`find_package(dannan 0.4 CONFIG)` takes it unchanged.

## 0.4.0: one settings reader for every node binary

Config: `dannan::config`, the layered settings reader every node binary shares, on
yaml-cpp (a new dependency). A binary declares its settings; the reader layers the
defaults, the file, `secrets.yaml` beside it, the environment and the flags, says
where each value came from, and reports keys nothing reads. `dannan-node` reads
through it, adding a file and `DANNAN_NODE_*` to its flags.

Encoding: percent-encoding in `dannan::encoding`, strict (RFC 3986) and form
decoding, and encoding by a set of literal bytes. Identity: `identity::x25519_public`,
`Nodes::certificate(node)` (the certificate a peer proved in its latest version 2
handshake) and `Identity::ca_certificate()`; `contract::Principal` compares with
`==`. The CMake package is `find_package(dannan 0.4 CONFIG)`.

## 0.3.0: identity on certificates

Identity: X.509 certificates through OpenSSL, as wire version 2 beside version 1. A
node proves itself by certificate, by the cluster key, or both while a cluster
moves, and refuses to start if its certificate does not name it, is not the CA's,
is out of date or is revoked. Revocations are signed by the CA, held, gossiped with
every member exchange and enforced: a new one closes the revoked node's sessions and
refuses its next handshake. `dannan-ca` (`init`, `issue`, `revoke`, `show`) and
`dannan-node`'s identity options give the operator's flow. The scope seam
(`Identity::install_scope_source`, `Identity::verify`) is declared here. The cluster
id left the hello's body, so 0.2.0 nodes do not meet 0.3.0 nodes.

For the hosted systems: `contract::Unlocked`, a scoped release of a held lock; a
call whose deadline passes is cancelled, freeing its reply slot and telling the
peer; `Time::source()` and `Time::executor()` on the Context; `sched::DutyShare`, a
background loop's duty cycle by weight; a membership event count that adverts do
not move; `Nodes::absent_since`, a node's absence kept across a restart; process
facts, thread names and timed spans. The CMake package is
`find_package(dannan 0.3 CONFIG)`.

## 0.2.0: Dannan's own wire, the node and level 0

Wire: Dannan speaks its own wire, version 1, in place of Macha's protocol 23, and
no longer interoperates with Macha. It has its own node record, telemetry, roster
and inbound files. User message types start at 512. The node, `dannan::Node` and
`dannan::Context`, brings membership and placement, identity, status and telemetry,
reach (inbound resolution, dial-back, public connectivity, UPnP) and supervision.
Handlers report whether they processed, failed or did not recognise a message.

Level 0: path classes, brokered punches and relays, so nodes behind NAT reach each
other; a relayed pair moves up to a punch when it can, CONTROL relay is
unconditional and DATA relay budgeted, and placement can keep relay-only nodes out
of the preferred owners. Channels: notify, stream and bulk, subscribed by topic
prefix. The roster-change feed, and each node's incarnation in its record.

Foundations for the stack: secure memory and buffers for key material, ES256 and
Ed25519 signing, Argon2id, node key custody under a systemd credential or a
passphrase, durable file creation that never replaces, JSON log lines, fuzz targets
for every untrusted parser, and shared test fixtures. Directories that durable
writes create are owner-only. Fixed: a call on a session retired off the route table
outlived `Client::stop()`. Installed for consumers as `find_package(dannan 0.2
CONFIG)` and `dannan::dannan`.

## 0.1.0: the transport on Asio

The first release: the vocabulary (locks, time, snapshots, work classes, retry,
logging, the retained-memory ledger and memory limit, the wire codec and durable
files) and the transport on Asio: the channel, sessions, the server and its request
scheduler, the client's routes and dials, health pings, dial backoff, broadcasts,
reverse dial and maintained peers. It speaks Macha's protocol 23 byte for byte,
proven in process against Macha's frames and in two processes against a Macha node.
