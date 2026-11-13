# Dannan for Macha, Lugh and Lir (2026-11-13)

Tom, 2026-11-13: "You all need to discuss your needs and ensure the the more
fundamental projects, namely Dannan and Dagda, support Macha, Lugh and Lir as
first class citizens. This was the purpose of splitting out the technologies."

This is Dannan's side: what each hosted system asked of Dannan, what Dannan
answered, and the proposals that need Tom's ruling. Nothing here is built until he
rules. The other sides are in `../lir/TODO/2026-11-13-lir-needs.md`,
`../lugh/TODO/2026-11-13-lugh-needs-of-the-stack.md` and the Dagda session's design;
Macha's answer is pending.

## Answered, no change to Dannan

- **The contract Lugh and Lir link** (Lugh N8, Lir N6). Nothing they link changed
  from 0.4.1 to 0.5.6. Identity (crypto, ES256 and Ed25519 signing, Argon2,
  custody), `config` and `encoding` have no diff at all. `context.h` only adds: a
  defaulted `CallWatch` on `call()`, `retire()`, `refresh_public_connectivity()` and
  `wait_guard()`. Two behaviours to know at a repin: a call is watched for progress
  only when it asks, and call deadlines follow injected time.
- **Adverts** (Lir N2, Dagda). The 64-advert cap stays, and Dagda aggregates its
  per-space adverts into one. The reason is the cost of a change, not the count. An
  advert change makes the node's whole record newer (`membership/membership.h:77-87`,
  `membership.cpp:185-195`), gossip carries the full record, and every peer persists
  it in its roster. A generation that moves with commits belongs on a notify channel,
  with at most a slow digest in the advert. To do: say so on `set_advert`.
  - Dagda's design takes it up (`../dagda/TODO/2026-11-13-three-systems-design.md`):
    only Macha's every-node space keeps its two adverts.
  - Every other space's generation goes on a notify channel named for the space,
    subscribed by its replica set.
  - The node advertises one slow key, `dagda.spaces`, which changes only on open or
    drop and fits in 1 KiB.
- **Argon2 under a sign-in storm** (Lugh N6). Hashing is foreground work (a person
  waits). Each hash reserves its `memory_kib` through
  `Context::budget().reserve(WorkClass::foreground, ...)` and answers "try later" at
  once when there is no room. It runs on Lugh's own workers, registered with
  `Context::threads()`, never on the node's executor.
- **Placement alignment** (Lir N5). `placement::Scheme` with the caller's salts is the
  shared piece. Keeping a replica set stable and aligning a lease holder with it are
  Dagda's.
- **The scope seam's real users** (Lugh N1). Dannan calls nothing through
  `ScopeSource` with a directory principal; only its own test does
  (`test/node_test.cpp:908`). Dagda is the real user. Every storing replica calls
  `Identity::verify` for incoming writes, claims, deletes and head commits
  (`dagda/store/distributed_store.cpp:697`, `:2471`; `store/storage_server.cpp:125`,
  `:356`; `heads/exchange.cpp:1036`).
  - The cross-node case is `StoreScope.AnInstalledSourceRefusesOnePrefixOnEveryReplica`
    (`dagda/test/store_scope_test.cpp:124`). A source on three nodes refuses
    `lugh/secrets` to a directory principal: every replica refuses with the source's
    reason and generation, and another prefix lands.
  - Claims are covered by `StoreScope.ClaimsAreCheckedAsWritesAre` (`:179`).
  - Lugh's `ScopeSource` will be the first real source.

## Proposals for Tom

### P1. An ABI promise in place of exact pinning (Lir N4, Lugh N9, Dagda)

Since 0.5.6 the SONAME and the CMake package accept only the exact version. So every
Dannan patch forces a Dagda release, and Macha, Lugh and Lir move in step.

Proposal:
- Patch releases keep the ABI. CI enforces it with libabigail's `abidiff` against the
  previous tag on both architectures, and fails on any break.
- The SONAME and package go back to major.minor (`libdannan.so.0.5`, `libdannan0.5`),
  and the CMake package to `SameMinorVersion`.
- An ABI change is a minor release, and only that forces the stack to rebuild.
- Each Dagda release names the Dannan minor it is built on. A hosted system pins one
  Dagda tag and takes that Dannan minor.

This reverses 0.5.6's exact pinning, which was right only while nothing checked the
ABI. Until Tom rules, a hosted system pins the Dannan its Dagda tag was built on.
Lugh, Lir and Dagda support it.

### P2. A partition reading (Lir N3, Lugh N10)

One reading on `Nodes`, which Lir, Lugh and Dagda use instead of each counting the
roster:
- the side: majority, minority or even;
- the known and reachable counts;
- the evidence: the as-of time and the node ids;
- a watch for changes.

The denominator is the roster as last agreed: it shrinks only by an operator's
retire, never by absence. A majority of the nodes merely reachable would let both
sides of a split call themselves the majority once each forgets the other after
`dead_after`. Questions for Tom:
- Is "even" a minority? Lir would treat it so, and a two-node cluster always splits
  even.
- How is the agreed roster changed: by `retire()` alone, or also by a node joining?

Lir degrades or refuses QoS 1/2 on a minority side, its own setting. Lugh reports the
side in Status and its audit log.

### P3. A shared public listener (Lugh, Lir; AthenaSIP likely)

The layer under the protocols: accept, TLS and DTLS, StartTLS, limits, deadlines,
stop and binding. Parsing HTTP, LDAP, MQTT, MQTT-SN, WebSocket and SIP stays each
hosted system's own. That keeps the stack's "Decided" 9 ("HTTP stays each hosted
system's own; there is no shared HTTPS stack"), but it moves the TLS and accept layer,
so the ruling is Tom's.

Lir has built its own listeners (`lir/edge/edge.h`, `lir/http/http.cpp`,
`lir/sn/gateway.cpp`), including a hand-written DTLS server, and would move onto a
shared one and delete them. Lugh runs its own HTTPS and LDAP listeners. The
requirements, combined:

1. It hands up a connected stream (TCP or TLS) or a datagram association (UDP or
   DTLS) on the caller's executor or strand, with the peer's address and the verified
   client certificate's subject and SHA-256 thumbprint (RFC 8705).
2. Certificates come from the operator's PEM files, per listener, and reload without
   a restart. Public clients do not trust the cluster CA.
3. The minimum TLS version is set per listener: Lugh's are TLS 1.3 only, and Lir's
   MQTT accepts 1.2 and up for devices.
4. Client certificates are per listener: required, optional, or never asked. A
   browser-facing listener must never ask.
5. StartTLS: a plaintext connection upgrades in place when the hosted system says so,
   reading nothing past the request until the handshake. Plaintext bytes after the
   request end the connection.
6. For HTTP, the trusted proxies' `X-Forwarded-For`, as a helper the hosted system
   applies.
7. Limits:
   - per address before a credential, and per caller after, shared across a hosted
     system's listeners;
   - the per-caller limit is an object the hosted system charges, and the listener
     consults it at accept;
   - accept rate, and a connection cap per listener.
8. Deadlines per listener (handshake and idle, on injected time), and a bounded clean
   close (`close_notify`; Lir bounds it at 2 s). Abnormal closes are immediate. A slow
   reader is closed past an outbound byte bound.
9. A stop that closes everything open.
10. Binding 443, 636, 389 and 1883 without root: an inherited socket-activation fd
    when given, else bind (with `CAP_NET_BIND_SERVICE`).

### P4. CPU admission by work class (Lugh N6, Dagda)

`Budget` is memory only (`context.h:280-289`). Dagda carries Macha's CPU pacing
(`store/cpu_pacing.h`: samples system and own CPU and scales background work to a
target), and Lugh needs CPU admission for hashing. The proposal is a class-aware
worker pool with that pacing in Dannan, starting from Dagda's `cpu_pacing.h`. Dagda
then links it.

### P5. Small-message figures (Lir N1, Dagda's commit rate)

Lir needs about 10,000 publishes a second, mostly under 1 KiB. Dannan has only bulk
figures. The proposal is to add to `dannan-bench`:
- notify fan-out to 1, 2 and 4 subscribers;
- calls carrying 1 to 64 envelopes a batch, as Lir's QoS 1/2 path does;
- payloads from 64 B to 1 KiB, at offered rates up to 20k/s;
- reports of msgs/s and p50, p99 and p99.9 latency.

It runs on the compose bed and loopback here. Two real machines (the Pis, or the
laptop against a Pi) wait for Tom's word. It also answers whether
`max_pending_replies` = 512 (`net/outbound.h:27`) limits Dagda's group commit. Lir
asks that it be measured before formats are fixed.

### P6. Macha's friction and exposures (Macha Server, 2026-11-13)

Nothing blocks Macha: 0.94.2 runs on 0.5.3 on both nodes. These are where the port
bends around Dannan, with paths in `../macha`. Most are small and Dannan's to fix.

1. `CallHandle::idle_for()` reads `max()` until the call is placed, so Macha converts
   it back to time since the call was made (`src/cluster/dannan_host.cpp:178-194`).
   Fix: report idleness from creation, or give the creation time.
2. Every request and notice is copied into Macha's message type
   (`src/cluster/dannan_host.cpp:21-26`, `:108-111`). Mostly Macha's to fix by
   adopting `net::Message`. Dannan's part: handlers may take the request by value or
   by move.
3. `Node::inbound()` has no const overload, so Macha const_casts
   (`src/cluster/cluster.cpp:650`). Fix: add it.
4. Thread names are mapped onto Macha's old ones (`src/cluster/cluster.cpp:655-661`).
   Nothing to build; Dannan keeps its thread names stable.
5. A supported test seam for owners. Macha built its own stall seam
   (`NodeRuntime::CallFault`, `src/cluster/cluster.cpp:345-346`) and a stand-in peer
   (`tests/stand_in_peer.hpp`), because `dial_hook` cannot stall or fail one call by
   type. Proposal: a per-call fault, stall or refusal by peer and type, and a bare peer
   node with chosen adverts and telemetry, in Dannan's test fixtures. It serves Lugh
   and Lir too.
6. Advert changes are not events. Roster watches carry joins, leaves, flags and
   replicas but not advert values, so Macha's survey notices a peer's growing `used`
   only on its next pass (`src/service/availability_service.cpp`). Proposal: an
   optional watch on a named advert key's changes, rate-limited. This is consistent
   with adverts being slow facts.
7. Logging blocks the thread that logs, io threads included, and Macha's sink writes
   the journal. A slow journal could stall io. Proposal: an asynchronous sink stage
   with a bounded queue that counts and reports drops, so io never waits on a log
   write. To measure on the Pis first.
8. The io thread count (Macha uses 2) has no measurement behind it. Proposal: add io
   threads to P5's measurement.
9. Test support's `free_port` collides across processes in the ephemeral range. Macha
   moved its own test ports below 32768. Fix: the same in Dannan's fixtures.
10. Packaging: done in 0.5.6. Macha moves to 0.5.6 with Dagda 0.1.5, and until then
    ships `libdannan.so.0.5*` and `dannan-import-macha` from the build tree. P1 would
    end exact-patch moves.
11. A peer's telemetry first arrives an interval after it joins. Macha fixed its
    survey's reading of "none yet" in 0.94.2. Proposal: send the first telemetry
    record at join, so no owner meets the race.

Item 12, the store cutover, needs nothing new of Dannan: Dagda registers Macha's store
numbers itself.

**Macha's function inventory, row by row against 0.5.3**
(`../macha/build/dannan-inventory-walk.md`). Of 95 rows, 37 are served, 8 served with
friction and 22 not served; 28 are out of scope (Macha's own, or store rows waiting
for Dagda).

Most rows not served are pieces Dannan has but Macha still runs as its own:
- placement, supervision, activity clocks and node events;
- the wait guard, walk budgets, retry, published snapshots, observation, durable file
  and injected time.
Macha retires those as its own work, some at the store cutover. The new friction:

12. Call watches. Macha zeroes the node-wide watches and polls its synchronous calls
    (`src/cluster/dannan_bridge.cpp:55-59`, `rpc_wait.cpp`). The per-call `CallWatch`
    on `Routes::call` is meant for owners; Macha moves onto it.
13. `connections_canonical`: the node leaves this core field unwritten, so Macha fills
    it from client stats (`src/cluster/cluster.cpp:168-170`). Fix: the node writes it.
14. Enum casts of `FrameType` and `WorkClass` (`dannan_bridge.hpp:50-55`): Macha's to
    remove by using Dannan's enums.
15. Two memory fits. Macha fits the limit itself and passes `memory_limit_bytes = 0`,
    so `fit_memory_limit` is unused (`dannan_bridge.cpp:71-77`). One fit should go,
    likely Macha's, once its holders are hosted owners.
16. Two durability epochs a process (`node_identity.cpp:45`): this goes with the store
    cutover.
17. Key-derivation labels are repeated by hand (`"dannan/auth/1"`,
    `"dannan/cluster-id/1"`, `dannan_bridge.cpp:81-88`), so a renamed label would
    silently split the cluster. Fix: export the labels as constants, or a
    `ClusterKey::from_master`.
18. Macha's `NodeId` and `Hash256` are now aliases of Dannan's (`src/types.hpp` on the
    cutover branch). Their layout, comparisons and `to_string` stay stable: part of
    P1's ABI promise.

Not decidable from reading, to be measured on the nodes:
- BBR and UPnP against a real gateway;
- whether the connectivity refresh's blocking of its caller is new. The asynchronous
  `refresh_public_connectivity` exists (`context.h:80-85`).
- whether the reassembly reserve, counted inside the memory total, leaves Macha's
  holders less room.

### P7. Alpine (Lugh, for Raven)

Built and passed on 2026-11-13, once, in a container here: Alpine 3.24.2 (musl), GCC
15.2.0, amd64, with Alpine's own Boost, OpenSSL, yaml-cpp and GoogleTest packages. The
build is RelWithDebInfo from `develop` at 0.5.6 plus the changelog; `dannan_tests`
gave 646 passed and 1 skipped (as root). No source change was needed.
- arm64 Alpine is not shown: this Intel Mac would emulate it, and the emulator
  misreports `posix_spawn`.
- Proposal: an Alpine step in hosted CI on both architectures, so musl stays proven.
  Raven is arm64 (a Pi 5, with Lugh's master key sealed under its OTP device key), so
  the arm64 Alpine step is the proof Lugh waits for. Lugh supports it.
