# Current release

## 0.10.1: a node's stop waits for a scheduled step; MemoryStore::each_held

A patch: the ABI is 0.10.0's, so a host built against 0.10.0 loads it.

Fixed: with a schedule (`NodeConfig::schedule`, the simulation's), a step that had passed its
check before the node stopped ran on while the node was destroyed. Hosted CI's TSan on 0.10.0
caught a roster step writing to a freed path. The stop now waits for a step already under way,
once it has released what that step might wait on. A node without a schedule was not affected.

Fixed: `tools/ci.sh install-debs` replaced the caller's `DEB_BUILD_OPTIONS` with `nocheck`, so a
caller's `parallel=1` was lost and the build ran on every CPU (Mimir's container, 1.5 GB on one
core, had its compiler killed). The caller's options are kept, with `nocheck` added.

Added: `agree::MemoryStore::each_held(slot, after, visit)`, the names a slot holds a value for,
in the order of their bytes, from strictly past `after`, until the visitor returns false (Mimir's
ask). Pages by the last name given, so a name removed between pages skips no other.

Adopting 0.10.1: a hosted system that keeps an in-memory acceptor store only to list what a
slot holds can drop it for `MemoryStore`. A test that worked round a node's stop racing its
scheduled steps can drop the workaround.

## 0.10.0: the Cluster Stack's first minor

A new minor: the ABI changes, so move a pin from 0.9.2 to 0.10.0 in one step (TODO/2026-11-20-stack-plan.md,
the first of three). Added:

- `contract/completion.h`: the stack's callback vocabulary. `Completion<T>` posts one answer to
  the caller's executor, never inline, by its deadline on a steady timer or an injected
  schedule; `Outcome<T>` with one `Fault` kind for every hosted system (none, not_found,
  conflict, duplicate, invalid, refused, unavailable, timed_out, failed). `contract/ack_level.h`:
  the four ack levels in order, with their standard names.
- `status::SwappableSink`, a root's sink from its first line with a lock-free filter and a
  race-free swap; `ScopedLogger::emit`, past the filter. `test::RecordingSink` keeps scopes.
- `agree::MemoryStore`, the production in-memory acceptor store.
- `dannan::testing`: the fuzz harness (`smoke`, `mutate`, `replay`, `run_all` on SplitMix64) with
  the simulation's checkers, installed with `fuzz/fuzz.h`; `dannan::sim_checkers` now names the
  checkers in a fetched build too.
- `RetryPolicy::unbounded(initial, ceiling)`, a policy that never parks.
- `identity::RandomId<Tag>`, a random 16-byte id, one type per kind.
- `identity::sha1` and `hmac_sha1`, for Valkey script ids and TOTP only.
- `identity::durable_create_directories` and `durable_sync_directory`; `durable_replace_file` and
  `durable_create_file` take the mode of the directories they make (0700 by default, as before).
- `encoding/base.h`: base64, base64url (padded or not), base32, exact decoders into any buffer
  (a secret into `SecureBuffer`), and `utf8_length`, `valid_utf8`, `printable`. `hex` and `unhex`
  move here; `identity::hex` and `identity::unhex` forward to them.
- `tools/ci.sh install-debs <tag>`: a dependant's CI builds and installs a release's packages in
  one step.
- `CODE_OF_CONDUCT.md`, the Contributor Covenant 2.1.
- `DANNAN_SANITIZE=address|thread`: instruments every target Dannan builds, fetched or not, and
  carries the flags to what links `dannan::dannan` in the build tree, so a consumer's sanitizer
  build never links an uninstrumented Dannan. `tools/ci.sh asan` and `tsan` build through it and
  check the library carries the runtime's symbols. `docs/building.md` has every option.

Changed:

- `listen::Prefix::parse` refuses host bits set past the length ("10.0.0.1/8" is now an error
  naming it, where it was read as 10.0.0.0/8 before) and a length that is not plain digits; an
  IPv4-mapped prefix or bare address is read as the IPv4 one, which `contains()` could never
  match before. `Prefix::to_string` and `listen::unmapped` are added.
- `fuzz_smoke_test` draws its inputs from SplitMix64, so they differ from 0.9.2's (the same on
  every standard library now).
- Fixed: an `Agreement` that went while a change still retried leaked its core, its served spaces
  and their stores, which held one another (Mimir's LeakSanitizer report on 0.9.2). Its end now
  closes each space, refusing what is queued, and a store's completions hold nothing of the space.
  `unserve` closes its space the same way.
- Fixed: a keep that a store completed after its `Agreement` had gone published the slot advert into
  the node's freed roster and answered the peer through the freed node (Mimir's ASan report on
  0.9.2). Once the `Agreement` has gone, nothing reaches the owner: no answer, no advert, no call.
- The library package is `libdannan0.10`. The README is about installing and using Dannan;
  why it exists and what it does moved to `docs/overview.md`.

Adopting 0.10.0: what a dependant can now delete or replace in its own code, and what to check.
Each is one home in the stack, so a copy kept elsewhere breaks the no-duplication rule.

- Your own callback result types, error enums or "posted, never inline" helpers: replace them with
  `Completion<T>` and `Outcome<T>` (`contract/completion.h`), and your ack-level names with
  `contract/ack_level.h`.
- A logger that drops lines written before the real sink is set, or swaps its sink under a lock:
  use `status::SwappableSink`. Tests that capture log lines by scope: use `test::RecordingSink`.
- An acceptor store kept in memory for tests or a single-node mode: use `agree::MemoryStore`.
- Your own fuzz driver, mutator or corpus replay: register targets with `dannan::testing`, and run
  them through `dannan::fuzz::smoke` in your own test binary. Your own copies of the simulation's
  checkers: link `dannan::sim_checkers`, now available in a fetched build too.
- A retry loop written by hand because `RetryPolicy` parked after its failure budget: use
  `RetryPolicy::unbounded(initial, ceiling)`.
- Your own 16-byte random ids: use `identity::RandomId<Tag>`, one type per kind of id.
- SHA-1 or HMAC-SHA1 code, or a dependency for it (Valkey script ids, TOTP): use `identity::sha1`
  and `identity::hmac_sha1`.
- `mkdir -p` plus fsync written by hand: use `identity::durable_create_directories` and
  `durable_sync_directory`.
- base64, base64url, base32, hex or UTF-8 checks of your own: use `encoding/base.h`, decoding
  secrets straight into a `SecureBuffer`.
- A CI step that builds Dannan's packages itself: replace it with `tools/ci.sh install-debs 0.10.0`.
- Sanitizer builds: build Dannan from source with `DANNAN_SANITIZE=address` or `thread`
  (`docs/building.md`), never against the Debian package, and check the library carries the
  `__asan_` or `__tsan_` symbols.
- Check: anything that parses an allow-list prefix with `listen::Prefix::parse` and wrote "a.b.c.d/n"
  with host bits set now gets an error. Write the network address instead.
- Delete: any teardown ordering you added to keep an `Agreement`, or the node under it, alive until
  your store's completions had run (posted turns before `node.reset()`, a store kept past the
  node). Both lifetime faults are fixed, so a store may complete after the node has gone.

Dependencies are unchanged: Boost 1.83 or later (Asio, headers only), OpenSSL 3 and yaml-cpp 0.7 or
later, Debian 13's own versions. No dependency moved in this release; reading their newer releases
against Dannan's code is planned work for the next.

## 0.9.2: an advert reaches a peer met after it was set

Fixed: an advert set before a node had met a peer, as when a hosted system advertises at
start while its bootstrap connection is still being made, was pushed to no one, and the peer
kept the older record the session's handshake carried until its own next exchange, a whole
heartbeat later (Lir, on 0.9.1). A node now sends its current record to each peer it meets
directly for the first time. `Nodes::advertise` with a deadline still answers only for the
peers known when it is called.

The ABI is 0.9.0's.

## 0.9.1: an advert change never ties a copy of the record already sent

Fixed: an advert set or cleared in the same millisecond as a gossip reply or handshake that
had carried the record before it got the same stamp, with new content, and a peer holding that
copy refused it, keeping only a newer stamp. `Nodes::advertise` and `Channels::subscribe` with a
deadline reported such peers as missed ("the peer holds an older record of this node"), and the
interest an advert carried could reach them only at the next change, so a message published
after a subscription was acknowledged could be lost (Lir, on 0.9.0). A change is now stamped
after every copy of the record already handed out.

The ABI is 0.9.0's.

## 0.9.0: agreement, the simulation harness, one file system, the hybrid clock

A new minor: the ABI and some of the API change (below). Move a pin from 0.8.x in one step.

**Agreement** (`Context::agreement()`, `dannan/agree/`): leaderless per-key agreement,
CASPaxos-shaped, over a caller-named replica set and slot (`max_slots` 8192): `change()`,
`change_deltas()` (deltas, a declined change, summaries, repeatable), `read()` with no accept,
`forget()`, `catch_up()`; reconfiguration between replica sets, the fast path checking ballot
and summary, value-less prepare. Acceptor state lives behind the hosted system's
`AcceptorStore` (`load`, `keep` answering `Kept{kept, need_whole}`, `value` answering
`Fetched{bytes, past}`, ancestry optional). After the review against the paper: a change is
never applied twice (ancestry, and `Outcome::unknown` when a retry cannot tell, failing
closed); a change naming several names in one slot is refused until atomic commit lands
(0.10.0); the round timeout is `agree::first_round`, doubling.

**The simulation harness** (`dannan/test/fixtures/sim/`, installed; link
`dannan::sim_checkers`, define `DANNAN_TEST_TEMP_ROOT` for `SimNodes`): a seeded world on one
thread; whole nodes in it (`SimNodes`, `host`, `restart(i, Wipe)`, `NodeClock` skew and drift,
`SimNetwork::partition` and `heal`) through `NodeConfig::schedule`, `NodeConfig::transport`
and `NodeConfig::fresh_id`; the worker pool on the schedule; clients as actors; and the
checkers: linearizability (Wing-Gong with Lowe's memo, models over groups of keys),
transactions by isolation level (Adya's cycles over list-append histories, with session or
real-time order), convergence for merge classes, durability per acknowledgement level.

**One file system** (`dannan/storage/file_system.h`): `FileSystem` and `File` returning
`Result` with the errno's class (`no_room`, `absent`, `io`, `exists`, `denied`, `busy`),
`or_throw()`, `list` with type and size, `stat`, `link`, `sync_directory`, `sync_all`,
`space`, an exclusive `lock`; POSIX by default, and `test::sim::Disk`, which crashes, lies,
tears and fills as a seed says. `durable_replace_file`, `durable_create_file` and `StateLock`
go through it.

**The hybrid logical clock** (`Context::hlc()`, `dannan/contract/hlc.h`): one per node,
48-bit milliseconds and 16-bit counter, big-endian on the wire, lock-free, never reissuing a
stamp across a restart (a persisted ceiling: the sealed counter `dannan/hlc` with custody, a
durable file without); `NodeConfig::hlc_max_ahead`, one minute; its counts and each peer's
skew on `Diagnostics::hlc`.

Also: `Nodes::advertise(key, value, deadline, executor, done)` telling who holds an advert
(`membership::Pushed`), and `Channels::subscribe` with a deadline (Lir's SUBACK hold).

Fixed: the 0.8 ledger's double free (Lir's TSan report); a durable write that made its
directories synced only the file's own, so a crash could lose a new directory and the file
in it (found by the simulated disk); a stopped node no longer runs what it had booked.

**Changed, for a caller moving from 0.8.x:**
- `CallOutcome::peer` is a `std::shared_ptr<const NodeRecord>`, null when no reply came: one
  record per session, no longer copied into every call (Dagda's profile).
- `net::InboundHandlers::request` takes that pointer; `RequestScheduler::submit`
  takes it too, with an overload that copies a record.
- `WorkerPoolConfig` has `schedule`; `NodeConfig` has `schedule`, `transport`, `fresh_id` and
  `hlc_max_ahead`; `Context` has `hlc()`; `Diagnostics` has `hlc`. Each defaults to 0.8.5's
  behaviour.
- `durable_replace_file`, `durable_create_file` and `StateLock` take a `FileSystem&`,
  defaulting to POSIX; `NodeIdentity` and `TelemetryStore` take an optional fresh id.
- Unchanged in shape: `listen::Backoff`, `StreamListener` and `Stream::start_tls` and
  `start_tls_unanswered`, `ClassGates`, `WorkerPool`'s interface.

**Moved to 0.10.0** (no stack project needs them yet): DTLS and UDP for `dannan::listen`;
the peer-witnessed high-water mark; atomic commit across replica sets (and several names in
one slot); loader promotion at the peer; the fuzz harness exported.

## 0.8.5: a sign-in's back-off, and a node destroyed after its io stopped

`listen::Backoff` (`dannan/listen/backoff.h`, header-only): failed sign-ins per key (a
principal, a source address), answered with a growing wait rather than a lock, so knowing a
user's name cannot lock them out. Some free failures, then a wait doubling up to a ceiling,
forgotten after an idle time; keys bounded by pruning the forgotten; on injected time;
thread-safe. Ask it before any costly check. Lugh's, moved into Dannan so Lugh and Kvasir
link one copy.

Fixed: a node stopped while its io ran, then destroyed after the io had stopped, hung in its
client's and its server's second stop, each of which posted to the stopped io and waited for
it (Louhi). A second stop now waits only for what is still in flight.

The ABI is 0.8.0's.

## 0.8.4: StartTLS with no answer, a stoppable wait for node events, and a deferred answer's slot freed in time

`Stream::start_tls_unanswered(done)`: StartTLS for a protocol that sends no answer, where
the client begins its handshake right behind its request (MySQL's SSLRequest, then its
ClientHello: Kvasir). Nothing is written and the bytes already waiting are taken as the
handshake. Every other check of `start_tls` stands: certificates present, once only,
nothing queued by `send()`. The hosted system must have read exactly the request.

`NodeEvents::wait_for_change(seen, timeout, std::stop_token)`: returns when the total
moves, when the token is stopped, or at the timeout; `milliseconds::max()` waits with no
timeout. A component's stop ends its wait at once, with no polling and no artificial kind
(Macha, whose NodeEvents become Dannan's).

Fixed: a deferred `Answer` sent its reply before giving back its `max_deferred` place and
bytes, so a caller that asked again at once could be refused as overloaded. Found by ASan
in hosted CI.

The ABI is 0.8.0's.

## 0.8.3: connecting out, and RS256 for WebAuthn

`dannan::listen`'s outbound side, `StreamConnector`: connects over TCP or TLS and hands
up the same `Stream` an accepted connection is, so a protocol's framing serves both ends
(Lir's MQTT client library, Lugh's publisher and HTTPS exchange, Mimir). One deadline on
the injected time covers resolution, connection and the handshake, a resolver that never
answers included; `done` is told once with the stream or why. TLS: `ClientTrust` reads
the CAs once (a named bundle, or the system's default file), so a bad bundle is found at
start and no handshake reads a file; the server's name is checked with no partial
wildcards, an IP literal against the certificate's addresses with no SNI, a trailing dot
allowed; a client certificate may be offered; there is no option that turns verification
off. TCP_NODELAY is on. `Stream::peer_certificate()` names the other end's certificate at
either end.

Identity: `verify_rs256(n, e, message, signature)`, RSASSA-PKCS1-v1_5 with SHA-256 under a
COSE RSA key (Lugh's WebAuthn), verify only: a modulus outside 2048 to 8192 bits, an even
or small exponent, or malformed input is false, never an exception.

Fixed: a sealed counters test that altered nothing one time in 256.

The ABI is 0.8.0's.

## 0.8.2: placement copies only its answer, and a listener's key is refused for what is wrong with it

Placement: `Nodes::placement` takes one copy of the roster and narrows it in place,
and `placement::by_capacity` ranks pointers into it, copying each record (adverts
and all) once, into the answer; before, one call copied every record about five
times. The fallback order hashes each node once rather than at every comparison.
The answers are the same. Measured by Dagda on a Pi 5 (200 files of 64 KiB, two
nodes): the placement copies are gone from the profile, and CPU per run is
unchanged within its spread, so this is no speed-up on that workload.

`dannan::listen`: a key that is not the chain's leaf's is refused as "not the key
of" the chain, not as "not a PEM private key", which OpenSSL's own pair check had
made it. A passphrase is never asked for: an encrypted key is refused as one,
where OpenSSL's default would prompt on a terminal and fail under systemd.

The ABI is 0.8.0's.

## 0.8.1: public sockets send at once, and a listener stops from anywhere

`dannan::listen`: every accepted socket sends at once (TCP_NODELAY), so a public
protocol's small packets never wait on Nagle behind the peer's delayed ACK, which
cost an MQTT PUBACK about 40 ms. `Stream::set_no_delay(false)` turns Nagle back on
for a stream; `Stream::no_delay()` reads it on the stream's executor.

`StreamListener::stop_accepting()`, and so `stop()` and the destructor, no longer
hang. It waited without a bound on the acceptor's strand, so it never returned
from a handler on a one-thread io context (a signal handler's place), after the io
context stopped (a node's usual teardown), or on one that never ran (a failed
start). Nothing is handed up once it returns; from a thread outside the io context
it still closes the acceptor first, waiting at most five seconds.

Stated: under optional client certificates a hosted system checks `verified`, not
only that a certificate is present.

The ABI is 0.8.0's; the ABI check now leaves out the standard library's template
instances, which a host instantiates itself.

## 0.8.0: work by class, a public listener, and counters that only go up

Scheduling: `sched::WorkerPool` runs tasks on supervised threads by work class
(control, foreground, loader, speculative). Each class has a bounded queue that
refuses at once with its reason, a cap on how many run, and workers held back for
the classes above. A queued task whose deadline passes, or that is cancelled, is
dropped and its caller told; tasks may own what cannot be copied. Loader and
speculative work is paced by `sched::ClassGate`, one per class per node, which a
long-running loop holds and every pool runs through, so a class never takes more
than its share. A background task rests by the CPU time it used while the rest of
the process is busy (`sched/cpu_pacing.h`, as Dagda's). The node owns the gates and
one pool (`Context::gates()`, `workers()`); a hosted system may run its own pool
beside it.

Public side: `dannan::listen`, the layer below a hosted system's protocols. It
accepts plaintext, TLS and StartTLS connections, and hands each up as a `Stream`
that Asio and Beast treat as any socket (`listen/beast.h` for the WebSocket). A
stream carries the peer, any client certificate with its chain verdict, and ALPN.
The listener adds:
- certificates that reload when the hosted system says, shared by its listeners;
- limits by listener, address and caller, all optional and bounded, with trusted
  proxies exempt by prefix;
- deadlines on injected time;
- a close that sends what was queued first;
- a stop in two steps, and systemd's sockets.
DTLS follows in 0.9.0.

Identity: counters that only go up, sealed beside custody (`Identity::counter`,
`advance_counter`, `advance_counters`, `note_counter`), for node-local state to refuse
a copy of itself restored from before. They refuse one directory restored alone, not
a whole state; the peers' witness follows in 0.9.0.

Memory: a hosted system waits on the node's budget for room, under a deadline on
the steady clock and a cancel flag (`Budget::acquire`, `wake`), and charges what it
holds to an owner of its own (`Budget::reserve(work, owner, bytes)`), which
`Budget::stats()` shows. An owner of Dannan's, or a wait without a deadline, is
refused.

Tools: `dannan-custody`, installed with a manual page, makes a node's key custody
under a systemd credential with the node stopped, printing the new root once for
`systemd-creds encrypt`, and shows the node's id and key once the sealed credential
opens it. Both refuse while a node holds the state; show writes nothing.

For hosted systems: a call refused or stalled by peer and type, and a bare peer, for
owners' tests (`NodeConfig::call_fault`, `test/fixtures/bare_peer.h`). A watch on one
advert's changes, rate-limited (`Nodes::watch_advert`). An asynchronous log stage that
never waits on its sink and counts what it drops (`status::AsyncSink`). Statics that
outlive a thread no longer leave it a destroyed clock. `DANNAN_BUILD_TOOLS=OFF` builds
the library alone. `dannan-bench calls` measures small messages: on one Pi 5 the call
path holds 12,000 calls/s at p99 under 1 ms. The ABI changes, so the library is
`libdannan.so.0.8` (`libdannan0.8`); a host is rebuilt once against 0.8.0.

## 0.7.0: an answer may come later, and a node knows its side of a split

Transport: a request handler may answer later, from any thread, holding no worker
while it waits. `Routes::bind_deferred` binds a handler that is given the request by
value and its `net::Answer`: it moves the Answer out, returns `Handled::deferred()`,
and gives it once, from a group commit's thread for example. An Answer dropped
ungiven refuses the call (500, "abandoned"), and deferred requests are bounded per
work class (`ServerLimits::max_deferred`, 4096) and keep their bytes charged until
answered. A handler bound the old way is unchanged. A deferring handler owns the
request, so it takes the payload without a copy.

Membership: `Nodes::partition()` says which side of a split this node is on, read
against the roster as last agreed: a majority only with more than half of it
reachable, so an even split is a minority. A node joins the agreed roster when it is
seen to join, and only `Nodes::retire()` (an operator's word) or an identity reset
takes it out, never absence. It is persisted beside the roster. `watch_partition`
reports each change. Nodes joined during a split can give both sides a majority: join
nodes to a whole cluster.

Fixes: a call cancelled before it is placed now completes at once, as its handle
says, rather than when its dial ends; "cancelled" means the caller stopped waiting,
not that the peer never acted. A call's `idle_for()` counts from when it was made,
its dial included. A request's timing is counted before its answer goes.

For hosted systems: `ClusterKey::from_master()` and the two cluster key labels, named
once; a const `Node::inbound()`; test ports from below every kernel's ephemeral
range; and a type is to be registered on every node that calls or answers it (said
on `Routes::add`). The ABI changes, so the library is `libdannan.so.0.7`
(`libdannan0.7`); a host is rebuilt once against 0.7.0.

## 0.6.0: the ABI is kept within a minor

Compatibility: from 0.6.0 a patch release keeps the ABI. The SONAME and the library's
package are major.minor (`libdannan.so.0.6`, `libdannan0.6`), and the CMake package
accepts any later patch of the minor it was asked for, so `find_package(dannan 0.6.0
CONFIG)` takes 0.6.0 and every 0.6 patch after it. CI compares every build's library
with its minor's last release by abidiff and fails on any change but an addition; an
ABI change waits for the next minor. A host's package depends on `libdannan0.6` at the
patch it was built with or later.

The promise starts at a new minor because the old SONAME cannot carry it. 0.5.0 to
0.5.5 were all `libdannan.so.0.5`, yet 0.5.4 changed the ABI: against 0.5.3 it removed
five functions, among them the old `reach::simultaneous_open`, and changed the type of
`NodeConfig::relay_drop_hook`. A host built against any 0.5 release, or 0.5.6's
`libdannan.so.0.5.6`, is rebuilt once against 0.6.0. The library's interface is
otherwise 0.5.6's.

Tests: a process started through the `ChildProcess` fixture no longer outlives a test
process that dies without running its destructors (a crash, an abort or SIGKILL).
Each child has a reaper that ends it when the test goes, and that never signals a
child already waited for.

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
