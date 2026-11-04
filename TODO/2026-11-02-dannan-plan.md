# Dannan: Distributed Substrate Library Plan

Written on 2026-11-03 from Tom's starting document of 2026-11-02. Since
2026-11-04 (Tom) this file is the master: the document it started from is
not read for direction or edited, and is to be deleted.

## What Dannan is

Dannan is the network layer, the virtual PHY and MAC: a C++20 library that makes a set of trusted nodes, on any mix of home hardware and rented cloud, behave as one medium that hosted systems transmit over. It owns reachability (including relay for nodes behind CGNAT), the two lanes and their channels, membership and capabilities, node identity and principal scope, placement as a pure function over the roster, and the work classes and budgets that keep control from waiting on data. It never pretends to be a single machine: every call carries a work class, a deadline and a principal, and every call returns evidence of where something happened, never a boolean.

Dannan stores nothing. Objects, claims, heads, leases and data domains belong to a separate library above it (the store, name to be decided), which is one peer among the hosted systems, not a floor under them. A node may run Dannan alone, as a pure radio that relays, gossips and holds the roster, with no store linked and no domain key present.

Hosted systems are peers on Dannan, not layers on each other: the store, Macha (media filesystem), AthenaSIP (SIP), Lugh (directory), and the planned KV store, event system, git forge, mail, compute platform and general filesystem. Each is a node-local process whose only peer is Dannan. Events and compute use Dannan directly and reach for the store only where they want durability. Hosted systems never address one another; they exchange frames through Dannan and, where they share state, through the store.

The membership test for what goes in: a thing is Dannan only if every hosted system needs it identically and none needs it in its own shape. The commit DAG's machinery passes; the merge function fails. The object store passes; "extent", "binding" and "topic" fail. What fails the test stays out permanently, which is what stops the KV store from becoming the floor that everything else is rebuilt on.

Dannan and the store are both extracted from Macha, which already implements most of each, and are proven against Macha first, then against AthenaSIP, which pulls from the opposite side (real-time, non-blocking strand, call-path latency). The extraction plan for Macha is its own document; this one specifies the two libraries, Dannan in full and the store as far as its boundary with Dannan.

## Consumers and what each needs

Eight hosted systems, one substrate. The table is the requirements list for Dannan: a noun appears in Dannan because a row needs it.

| Hosted system | Mutable state (heads) | Bulk (objects) | Channels | Leases / claims | Node-local, never shared |
|---|---|---|---|---|---|
| Macha | namespace, catalogue manifest, torrent requests | extents, artwork, control shards | metadata notices, trie diff | torrent claim epochs, retention claims | FUSE spool and journal, playback sessions |
| AthenaSIP | realms, subscribers, bindings, trunks, admin users, call records, presence | none today; call recordings later | node status (retained), presence/<aor>/changed (signal, lossy by design), call lifecycle | trunk registration lease, presence takeover lease, orphan-call sweep, rate-limit counters | connections, flows, in-flight transactions, dialog signalling, flow tokens |
| Lugh | principals, groups, grants, revocations | none | change notices | uniqueness-at-creation claims | sessions |
| KV store | namespaces of small values | large values | change feed per namespace | none | derived indexes |
| Event system | retained topics, subscriber lists, persistent sessions | large payloads | QoS 0 fan-out (CONTROL, at-most-once), QoS 1 (at-least-once, per-origin sequence) | shared-subscription group ownership, client-ID uniqueness | live subscriber tables |
| Git forge | refs, repo metadata | packs, loose objects | ref-change notices | push serialisation per repo | pack caches, FTS of commits |
| Mail | mailbox op-logs, identity records | message bodies, attachments | inbox queue signals | outbound send claim | FTS index |
| Compute | task definitions, desired state, service records | images, logs | change feed, health | task placement claim with fencing | Docker driver, overlay interfaces |
| General FS | inodes, directories | blocks, extents | lease revocation | write leases per inode | page cache, NFS/kernel edge |

What AthenaSIP teaches that Macha does not. AthenaSIP today keeps every shared record in one central Redis (src/datastores/redis_datastore.cpp) and signals change over one central Mosquitto (src/events/mqtt_event_system.cpp), with Lua EVAL for leases and counters. Both are single points of failure the design already treats as such (TODO/ACTIVE.md: state lives in the datastore, the bus signals and is not storage). The port replaces Redis with heads and the bus with channels, and the datastore interface (src/datastores/datastore.h) is the port boundary: lease and counter_add become Dannan leases and per-origin counters; location_list and friends become reads of a head the node already holds.

AthenaSIP also sets the latency bar. All signalling runs on one non-blocking strand; datastore reads sit on the call path (binding lookup, nonce check); RFC 3261 Timer A fires at 500 ms. So Dannan's local read of a head must be a memory read, writes must be acceptable locally and replicate asynchronously, and every Dannan call from a strand must be posted back to the caller's executor, never completed inline. That rule already exists in AthenaSIP's plugin contract (docs/plugins.md) and becomes Dannan's.

AthenaSIP also already has a cluster CA (src/cluster_ca.cpp): EC P-256, node certs with SANs, mutual TLS on the inter-node listener. That is the node-identity model Dannan adopts; Macha's single shared cluster key becomes the degenerate case of a CA with one node class.

And it already has the reachability problem this plan solves at level 0: src/address_discovery.cpp probes peers every 10 minutes, a node nobody can reach marks itself reachable:false and stops receiving forwards, and one of the two live nodes (corvus-gbni-1) sits behind NAT. There is no relay between nodes. Dannan's relay path removes that limitation for every hosted system at once.

## Level 0: reachability

A node that cannot be dialled is a full member. Reachability is a property of a path between two nodes, not of a node, and nothing above level 0 can observe which path is in use.

Three path classes, tried in order per pair:

1. Direct. Either side has a public address the other can dial. Macha's inbound_capable: false already covers the half-case: the NAT'd node dials out and keeps both lanes open, and a peer that needs a lane asks over the CONTROL session the NAT'd node opened.
2. Punched. Neither side is dialable but at least one NAT is not symmetric. A public node both already talk to brokers a simultaneous open on CONTROL: it tells each the other's observed (address, port) and a time, both send, the first packet through wins. No SDP, no ICE agent; the candidates are the observed addresses Dannan already learns from every inbound frame (the received/rport idea from SIP, which AthenaSIP already does in address_discovery.cpp).
3. Relayed. CGNAT or symmetric NAT on both sides. A public node with the relay capability forwards frames for the pair. SecureChannel is end to end, so the relay carries ciphertext and learns only that A and B talked and how much.

Rules:

- Membership gossips each node's reachability class (public, nat-punchable, relay-only), its observed candidate addresses, and which nodes offer relay. Observed addresses are confirmed only when another node has reached them, as AthenaSIP does with its reaches probe.
- CONTROL relay is unconditional. A member must always be able to gossip, commit, repair and hold leases; the cost is small and refusing it would make the node a second-class member.
- DATA relay is budgeted. The relaying node charges relayed bytes to its own loader class and publishes relayed bytes per pair in Status. Placement prefers direct and punched paths for bulk, so a relay-only node still receives objects but is not a preferred owner for others' hot data.
- A path change is a transport reconnect under the same node identity. ClusterNode is unchanged, in-flight RPCs retry under the existing policy, lanes keep their class. Punched-to-direct, relayed-to-punched and the reverse are all the same event.
- Node identity is the certificate, never the address. A node that moves from fibre to 4G keeps its node_id, its claims and its placement rank.

Limits, stated so they are not mistaken for bugs:

- A cluster with no public node and no punchable NAT cannot form. One bare public node is the minimum, which is already the case today; level 0 removes a dependency rather than adding one.
- The relay is a correlation point. Trusted nodes only; this is not an anonymity property.
- Relay-only nodes see higher latency on bulk. The hosted system is told the path class per peer if it asks, so a latency-sensitive placement (AthenaSIP's binding owner, a compute task) can prefer peers on direct paths without level 0 leaking into its logic by default.

DNS. Dannan has no namespace and no resolver; bind9 serves. What Dannan and the store enable is an eventually consistent zone: a zone is a store head with a merge rule DNS already tolerates (LWW per RRset, durable conflict on a same-RRset race), fed to bind9 by zone-file write plus rndc reload, by nsupdate (RFC 2136), or by a DLZ backend reading the head. A hosted system publishes a node's addresses and capability SRV records into the operator's own zone, under the operator's domain; Dannan never chooses a name. A network-only node runs bind9 as a secondary via AXFR/IXFR from a member, so a cheap public node answers for your domain from DNS's own replication, not the store's. Bootstrap is a seed name in that zone resolving to relays; Macha's bootstrap list and AthenaSIP's SRV failover both read it.

## Level 1: lanes, routes, channels

Two lanes per peer, CONTROL and DATA, as Macha has them (cluster/net.hpp). CONTROL carries membership, heads, leases, notices and small RPC; DATA carries object payloads with backpressure. Control never waits on data (law 1). A hosted system cannot open a third lane; it gets a channel on one of the two.

Typed routes. MessageRoutes (cluster/message_routes.hpp) keeps its handler table, but the message-type space opens: Dannan reserves a range for itself and a hosted system registers its own types at start, each with a lane and a work class. Existing Macha values keep their numbers so the protocol does not move during extraction. A registration is refused if it collides or names a reserved lane class.

Channels. A channel is a named route plus a delivery contract chosen at open and never implied:

| Contract | Lane | Semantics | Who uses it |
|---|---|---|---|
| notify | CONTROL | at-most-once, no sequence, dropped under pressure | Macha metadata notices; AthenaSIP presence/<aor>/changed; MQTT QoS 0 |
| stream | CONTROL | at-least-once, per-origin sequence, bounded replay from the sender's buffer | change feeds; MQTT QoS 1; AthenaSIP call lifecycle to an API consumer |
| bulk | DATA | at-least-once, backpressured, object-sized frames | repair, trie diff, pack transfer, image pull |
| retained | CONTROL + a head | the last value per key, delivered on subscribe | node status; MQTT retained topics; AthenaSIP nodes/<id>/status |

There is no exactly-once contract and no ordering across origins. A retained channel is a notify channel whose last value also lands in a head the hosted system owns, which is how AthenaSIP's retained status and MQTT's retained topics stop needing a broker.

Fan-out. Subscription is node-local: a hosted system subscribes on its own node, and Dannan forwards to the nodes that have subscribers, learned through gossip. A notify to a topic nobody subscribes to costs nothing. This is the event system's substrate, not the event system: the broker adds MQTT sessions, wildcards, wills and shared subscriptions on top, in its own code.

Executor rule. Every Dannan completion is posted to the caller's executor, never run inline. AthenaSIP's strand depends on this; Macha's supervised threads tolerate it. A hosted system that needs synchronous reads reads a head it already holds in memory, which Dannan guarantees is a memory read.

## Membership, capabilities, placement

Membership is Macha's gossiped roster (cluster/membership.hpp) unchanged in mechanism: direct-versus-gossip liveness, dead_after, forgetting of nodes not heard from, identity resets. AthenaSIP's node_directory (retained MQTT status, will, three-interval staleness) is retired in favour of it; the two agree on semantics already, AthenaSIP just rents a broker to do it.

Capabilities replace Macha's two flags hosts_extents and inbound_capable with a gossiped set per node. Dannan defines store.data, store.control, relay, and the reachability class; a hosted system adds its own (macha.fuse, athena.sip, athena.media, compute.docker). A bare Dannan node advertises only store and relay. Placement for durability asks "any node with store.data"; placement for serving asks "nodes with X". A hosted system never learns a node's address or path class unless it asks.

Placement is cluster/placement.hpp as it stands: rendezvous hashing, capacity-aware placement over the 2^32 virtual shard space, failure-domain spread, latency-ordered replica fan-out. All pure functions over bytes and the roster; Dannan exposes them to hosted systems so that AthenaSIP's "hash the Call-ID to pick an rtpengine", the forge's "which nodes serve this repo" and compute's "where does this task run" stop being three private implementations.

Failure domains come from the roster, not configuration guesses: a domain is declared per node (home-fi, hetzner-hel1), and replicas are spread across domains first. The sovereignty rule is expressible here and only here: a hosted system can require that its write floor be satisfiable within a named set of domains, so a cluster never depends on rented nodes for durability.

## The store: a separate library

Everything from here to the end of Leases is the store, not Dannan. It is Macha's storage, ledger and metadata machinery as a library that links Dannan and is itself a hosted system: one peer among the four, with inherently immutable content (extents addressed by their 256-bit hash), owner-supplied codecs and merges, and data domains. A node without the store linked holds nothing, relays everything, and is never a placement candidate. The store's name is open.

## Domains

A domain is a symmetric key plus a namespace. Every object, control object and head belongs to exactly one domain; keys.storage becomes storage[domain]. A node holds a set of domain keys and is, per domain, one of three things advertised as a Dannan capability:

| Role | Has key | Stores | Reads, merges, serves | Counts toward durability |
|---|---|---|---|---|
| member | yes | yes | yes | yes |
| blind | no | when directed | never | yes |
| none | no | no | no | no |

A bare VPS is blind for every domain it is asked to hold. Macha today is one domain with every node a member, which is the degenerate case and keeps the extraction byte-compatible.

The one real change is object identity. ObjectId is SHA-256 of plaintext, so a blind node cannot verify what it holds. Each stored record therefore also carries a ciphertext digest (the pack record's AES-GCM tag already serves) bound to the plaintext id by the member that sealed it; a blind node verifies the ciphertext, a member verifies both. has(id) and trie diff keep working on the plaintext id because a blind node needs only to know which id it holds.

Placement gains a domain filter: durability over member ∪ blind, serving over member. Write floors and replicas are per domain, so "two member copies at home, one blind in the cloud" is one config line, and a domain can require its floor be satisfiable within named failure domains.

Blind replicas store commits and count toward the metadata write floor, but merge and conflict materialisation happen only on members, and a head space whose members are all offline is readable by nobody. Acceptance certificates name which replicas were blind. Claims and GC need nothing: dots are per id.

Domain keys are distributed out of band, or wrapped to member node certs so a new member is enrolled by a signed grant. Rotation re-seals one domain, not the cluster.

Limits: a blind node learns ids, sizes, access patterns and domain membership (metadata, not content); and a domain whose members are all gone is lost regardless of blind copies, so at least two members in two failure domains is the rule that keeps a cheap node strictly additive.

## Store: objects, tickets, claims, GC

Objects are immutable bytes identified by SHA-256 of plaintext (ObjectId = Hash256, types.hpp). Encryption and packing are node-local and never change the id. Two stores with separate quotas, as Macha has them: DATA for bulk and CONTROL for small objects a head needs to name; DATA exhaustion cannot block a CONTROL write. DistributedStore (cluster/distributed_store.hpp), LocalStore, StoragePool and the pack format move unchanged.

put returns evidence: the set of nodes that hold the object, which tier each is on (provisional, durable, cached), and a ticket. put_deferred plus durability_barrier batch the syncfs as today. get returns bytes and which node served them. has(id) is the ground-truth probe that every disciplined check falls back to (discipline 1, re-derive don't assert).

Tickets are DurabilityDomain generations (storage/durability_domain.hpp), fenced by process epoch and backend incarnation. A hosted system may wait on a ticket, be told it was lost, or ignore it; "accepted locally" is a legitimate state and the hosted system chooses what to wait for. Macha waits for min_write_replicas before naming an extent in a commit; AthenaSIP will wait for nothing on the call path and for one replica before confirming a REGISTER.

Claims are (origin, sequence) dots per object (contract/claim_store.hpp, storage/retention.hpp). A hosted system says "I still need this" by claiming; it never tells Dannan what the object is. Release may remove only dots dominated by the releaser's RetentionClock, so a claim from an unseen branch survives a partition automatically. Reachability for GC is supplied by the hosted system through a ReferencedSets provider, which is the seam that takes catalogue/ out of ledger/node_horizon_builder.hpp.

GC keeps Macha's two sweeps (DATA and CONTROL), the grace period, and the rule that a physical copy with any local claim is untouchable. Deletion is reachability-safe and lazily durable, because a lost unlink can only preserve garbage.

Anti-entropy is the ObjectTrie and trie_diff from ledger/: each store's holdings as a radix-256 Merkle trie, compared in rounds of prefix queries, repaired over bulk. This is the single most reusable piece in Macha and is exposed to hosted systems as-is, so a KV namespace or a mailbox can diff its own holdings the same way.

## Store: heads, the opaque commit DAG

A head is the only mutable thing in Dannan, and Dannan does not know what is inside it. A hosted system opens a named head space (macha/namespace, athena/bindings, lugh/principals, kv/<namespace>) and supplies three things: a codec for its state blob, a merge function (base?, a, b) -> state | conflict, and a reachability provider for GC. Dannan supplies everything else, which is Macha's metadata machinery (metadata/metadata_manager.hpp) with the FS state type removed:

- commits are immutable, content-identified, full or delta, with one or more parents;
- a commit is complete when metadata_min_write_replicas distinct active replicas hold it and an acceptance certificate is persisted. No vote, no prepare phase. Every replica counts equally;
- accepted heads are a set. A disconnected cohort produces a valid branch; replicas exchange head certificates and compact ancestry; on divergence Dannan materialises both and calls the owner's merge; an owner that returns conflict gets a durable conflict record, never an overwrite;
- per-origin dots and clocks make retries idempotent;
- history is compacted out of the hot path (discipline 4); a snapshot's size is a function of live state;
- large state uses the content-addressed Merkle tree over keys that macha-namespace-migrate introduced, so a commit rewrites one leaf and the branches above it.

Reads are local. Every node holding a head space has its current head set in memory. A read is a memory read with no network call, which is what lets AthenaSIP look up a binding on the call path and lets any node answer a KV GET alone.

CAS is cas/cas_delta on generation and hash (metadata/metadata.hpp), exposed as mutate(space, fn) with optimistic retry. It is linearizable on one node and not across nodes; two partitioned nodes can both succeed, and the merge sees both. The API says so in its name: mutate_local, never cas.

Scope per space. Not every node needs every head. Macha replicates its namespace to every node; AthenaSIP's call records and Lugh's directory want the same; a KV namespace or a git repo's refs may want three replicas. A head space declares its replica policy on open, and placement for CONTROL decides which nodes hold it. A node that does not hold a space forwards reads over CONTROL and says so in the evidence.

Macha compatibility. The FS's codec is defined as the existing SM15–18 and DLT9/10 encodings, so Macha's layout v18 and protocol 23 survive extraction unchanged; the first generic format is FS-shaped by grandfathering, and a layout bump is reserved for the first state the FS encoding cannot carry.

## Store: leases and claimed collections

A lease is a soft claim with an epoch and a fence, lifted from Macha's torrent-request CRDT (torrent/torrent_request.hpp) into a template. It answers "who is doing this" without consensus:

- acquire(name, scope) -> {holder, epoch, since}; the record lives in a head the hosted system owns, merged field-wise: higher epoch wins, then earlier claim, then lower node_id;
- a lease holds while its holder is a member; after absence another node takes over at epoch + 1; a holder that returns and finds a higher epoch stands down;
- every action taken under a lease carries its epoch, and the receiver rejects a lower one. That fence is the whole guarantee: two holders can coexist for one absence window, and the stale one cannot act.

The claimed collection is the same template over a map: field-wise LWW by (changed_ms, node_id), a dominating cancelled, a max-wins tombstone with a retention window, three-way merge over the key set, a duplicate rule per hosted system (Macha's "same info hash, earlier wins, later cancelled"). TorrentRequest becomes its first instantiation with identical bytes.

What moves onto it from AthenaSIP: Datastore::lease (datastores/datastore.h:151, Redis EVAL at redis_datastore.cpp:449) and its three users — trunk registration (trunk_registrar.cpp:160, two-minute takeover), presence subscription takeover (presence_list_node), and the orphan-call sweep, which today elects the lowest live node_id in core.cpp and becomes an ordinary lease on athena/sweep. counter_add becomes a per-origin counter merged by sum, which is what a rate limit needs and all a Redis INCR was giving it.

What moves onto it later: MQTT shared-subscription group ownership and client-ID uniqueness; the mail outbound send claim; compute task placement; FS write leases per inode; the forge's push serialisation per repo. Each is a name, a scope, an absence window and an action that carries the epoch. None is a lock.

## Work classes, budgets, faults

The four laws and five disciplines (docs/principles-and-laws.md) move into Dannan verbatim and apply to every hosted system. They are the scheduling model, not a style guide.

Work classes are control, foreground, read_ahead, loader, speculative, carried on every wire frame (cluster/frame_type.hpp) and every call. "Viewer" is a hosted-system word; Dannan says foreground. The class comes from the hosted system at the call site and is propagated by Dannan through every queue, lane and store it touches. AthenaSIP's call path is control and foreground; its push refresher and presence reconcile are loader; Macha's repair is speculative.

Budgets are the three memory reserves (runtime.*_memory_reserve_bytes), I/O credit, and cache slots, handed to a hosted system by class at start. Discipline 5 is enforced at admission: a hosted system declares its unit of work per class, and Dannan refuses to start it if a budget is smaller than one unit, rather than letting it run at zero cache and 28 bytes per second. RetainedMemoryLedger (retained_memory.hpp) is the accounting; Budget/Cursor/Page (contract/walk.hpp) are how long work is paged under it.

Per-account bounds (law 2, second clause) are a Dannan noun, not a convention: every per-principal resource has a bound beside the node-wide one. AthenaSIP's rate_limiter.h and Macha's per-session limits become instances.

Faults are values. A hosted system attaches a fault sink; Dannan's supervisor (subsystem/subsystem_supervisor.hpp) restarts it with backoff until a policy disables it. A node that cannot recover state refuses to start and says why, in the operator's terms (discipline 3). No RPC waits without a deadline (discipline 2), and the parked state is visible in Status.

Time is injected (TimeSource), so a hosted system's timers and Dannan's leases are testable under the same clock, and clock skew is a chaos-monkey move rather than a surprise.

## Identity and scope

Two kinds of principal, one check.

Nodes hold a certificate from the cluster CA. AthenaSIP's cluster_ca.cpp is the implementation: EC P-256 CA, pathlen 0, two-year node certs with SANs naming the node_id, mutual TLS on every inter-node connection, dialled address checked against the SANs, ca.key offline. Macha's single shared cluster key becomes a CA with one node class; SecureChannel keys from the cert instead. A node is any peer presenting a CA-signed cert; there is no further node authorisation, because nodes are trusted (the non-Byzantine assumption, stated).

Users and services are Lugh principals. Lugh is itself a hosted system on Dannan, so its directory is a head space every node can read locally. A writer presents a credential Lugh issued; the credential names the principal and its scopes, where a scope is a head-space prefix and an object namespace the principal may claim into.

The write-floor check. A commit or claim carries its principal. Each replica that counts toward the write floor checks the principal's scope against the head space before storing. A commit outside scope is refused by every honest replica, so it can never reach the floor. Revocation is a Lugh head change; because reads are local, every node sees it within one replication round, which is the "sessions retire cluster-wide on credential change" behaviour Macha already has and AthenaSIP gets for free.

Open between the two models: scope carried on the credential (partition-safe, revocation lags until the credential expires) versus looked up in the Lugh head at write time (immediate revocation, needs the node to hold the Lugh space). The plan takes the second as default, with a short-lived credential as the fallback for a node that has lost the Lugh space; it is listed under open decisions.

Subscriber authentication inside AthenaSIP (SIP Digest, HA1 per algorithm) and admin bearer tokens are hosted-system matters and stay in AthenaSIP; what changes is where the HA1 and the session are stored.

## The hosted-system API

dannan::Context is what a hosted system receives at start and the only way it touches the network. It is Macha's SubsystemContext with everything storage-shaped removed and the missing network nouns added. The store exposes a second context, store::Context, built on Dannan's, for hosted systems that want durability. Both are written as specs so that a second implementation could satisfy them; that is the test that each boundary is real.

| Library | Noun | Calls (all take WorkClass, Deadline, Principal; all complete on the caller's executor) | Returns |
|---|---|---|---|
| Dannan | Nodes | roster(), self(), capabilities(node), path_class(node), placement(key, policy, filter) | roster snapshot; placement = ordered candidates with fallbacks |
| Dannan | Routes | register(type, lane, class, handler), call(node, type, bytes), call_async, broadcast_notify | replies as today in MessageRoutes |
| Dannan | Channels | open(name, lane, contract), send(ch, frame), subscribe(topic, filter, handler) | accepted/dropped and, for stream, the per-origin sequence |
| Dannan | Identity | self_cert(), verify(principal, scope), scopes(principal) | local read of the Lugh space when the store is linked; cert-carried scope otherwise |
| Dannan | Budget | declare(class, unit_of_work), reserve(class, bytes), credit(class, io) | refused at start if a budget is below one unit |
| Dannan | Faults | sink(handler), raise(fault) | restart or disable per policy |
| Dannan | Time | now(), after(duration, fn) | injected clock |
| Dannan | Status | publish(key, value), counter(name) | surfaces in the node's Status page |
| Store | Domains | join(domain, key?), role(domain), members(domain) | member, blind or none; advertised as a Dannan capability |
| Store | Objects | put(bytes, domain, store), put_deferred, barrier(), get(id, domain), has(id), holdings_trie(domain, store) | Evidence{holders[], role and tier per holder, ticket}; get adds the serving node |
| Store | Claims | retain(id, dot), release(clock), register_reachability(provider) | ack with local claim state |
| Store | Heads | open(space, domain, codec, merge, policy), read(space), mutate_local(space, fn), commit(space, blob, parents), heads(space), conflicts(space), resolve(space, conflict, blob) | read is a memory read on a member; commit returns Evidence and a certificate once the floor is met |
| Store | Leases | acquire(name, scope, absence), renew, release, holder(name) | {holder, epoch, since}; every lease-guarded call takes the epoch |
| Store | Tickets | wait(ticket), status(ticket) | durable / pending / lost |
| Store | Retained | retain(topic, value) | a notify channel whose last value lands in a head |

Rules that every hosted system inherits, and that Dannan enforces rather than documents:

- Hosted systems never address one another; the only shared things are objects, heads and channels.
- No call returns a bare boolean for anything that happened on more than one node.
- No call waits without a deadline; a deadline that passes returns what was achieved.
- A hosted system is one process per node, supervised, with a declared unit of work per class.
- HTTP and client protocols are the hosted system's; Dannan offers the lane classifier and per-account bounds and nothing else about HTTP. Macha's HttpServer route registration is the FS's, not Dannan's.

## Repository, build, versioning

Dannan is its own repository from the start (Tom, 2026-11-03: Dannan is a new project, not an extension of Macha), with its own wire versioned from 1. Macha and AthenaSIP are ported onto it and pin its tags. The layout below is the original sketch; the repository's is in `CONTRIBUTING.md` (Layout).

```
dannan/                              the network layer
  include/dannan/      public headers: Context and its nouns, nothing else
  src/
    reach/             level 0: path classes, punch broker, relay
    net/               lanes, SecureChannel, routes, channels     (from cluster/net.*, message_routes.*)
    membership/        roster, liveness, capabilities, domains    (from cluster/membership.*)
    placement/         rendezvous, capacity, failure-domain spread (from cluster/placement.*)
    sched/             work classes, budgets, laws                (from contract/work.*, retained_memory.*)
    supervise/         subsystem lifecycle, faults                (from subsystem/)
    identity/          CA, node certs, principal and scope check  (from athenasip/src/cluster_ca.*)
    status/            counters, parked state, Status surface
  contract/            abstract interfaces with declared waits     (from contract/, network half)
  test/                gates, the chaos scenario runner

<store>/                             the store, separate library, links dannan
  src/
    domain/            domain keys, roles, blind verification
    store/             local stores, packs, pool, durability      (from storage/)
    ledger/            trie, diff, claims, horizons               (from ledger/, storage/retention.*)
    heads/             commit DAG, certificates, head sets        (from metadata/, minus FsEntry and the merge)
    lease/             lease and claimed-collection templates     (from torrent/torrent_request.*)
  contract/            object store, claim store, metadata view   (from contract/, storage half)
```

- C++20, CMake, same toolchain requirements as Macha. Boost.Asio for I/O because AthenaSIP already depends on it and Macha's own loop can sit on it; this is the one place the two codebases must agree before the port.
- Hosted-system ABI is the C++ ABI with a build-identity check, as both Macha (macha_subsystem_entry) and AthenaSIP (API_VERSION) do today. No stable C ABI is promised; a hosted system is rebuilt with Dannan.
- Wire protocol version is one number for the whole cluster; mismatch fails the handshake. Dannan owns the number; a hosted system's own messages are versioned inside its registered types.
- State layout is versioned and fresh-genesis: unversioned or mismatched state is refused, never adopted.
- Macha and AthenaSIP pin Dannan tags, as Dagda, Lugh and Lir do.

## Roadmap

Order of proof, each step gated by the chaos cluster.

1. Harness. Twelve nodes across two providers, most behind NAT, chaos monkey with terminate, partition, volume detach and clock skew, the correctness gates asserted nightly. Everything after this is measured against it.
2. Extract from Macha by the eight seams in the extraction plan: CMake boundary, open route space, HTTP registration, capability set, lease template, reachability provider, opaque heads with grandfathered FS codec, Context as the API. Macha's experience unchanged; no disk or wire byte moves.
3. Level 0. Punch broker and relay as new code in reach/, proven on the chaos cluster by making half the nodes relay-only and asserting the same gates. This is the first thing Dannan has that Macha did not.
4. Identity. Port AthenaSIP's cluster CA into identity/; Macha's shared key becomes a one-class CA; SecureChannel keys from certs. Rolling, under a protocol bump that is the first and only one in this plan.
5. AthenaSIP port. Implement Datastore and EventSystem as Dannan-backed drivers behind the existing plugin interfaces (datastore.h, event_system.h), leaving the SIP stack untouched: heads for realms, subscribers, bindings, trunks, admin users, call records and presence; notify channels for signals; leases for trunk registration, presence takeover and the sweep; per-origin counters for rate limits; retained status from gossip. Redis and Mosquitto are then optional and, once redis:// and mqtt:// are no longer in any deployment, removed. Gate: the existing cluster e2e (test/e2e/cluster.sh) passes with the second live node behind CGNAT and no public address.
6. Split. Dannan becomes its own repository; Macha and AthenaSIP pin a tag. Lugh moves onto it when its owner says so.
7. The six. Order by how differently each pulls on the door: event system (channels, retained heads, no objects), KV store (heads with user merges, trie diff per namespace), git forge (objects at scale, push leases), compute (claims with fencing, capability placement, the overlay), mail (op-log heads, outbound claims, gateway protocols), general FS (write leases, kernel edge). Any core change one of them needs is a seam proven on Macha and AthenaSIP first.

Claude Code sessions map one-to-one onto numbered steps and seams; each gets a CLAUDE.md handoff naming the gate it must leave green.

## Limits by design

Each is a boundary, not a gap, and the API is shaped so a programmer cannot forget it.

- No consensus, quorum, leader or linearizable operation. A lone node accepts writes; branches merge later; mutate_local is named for what it is.
- No uniqueness guarantee. Concurrent creators both succeed and the owner's merge decides; Dannan guarantees only that neither is lost.
- No locks or transactions. A lease is a soft claim with an epoch; two holders may coexist for one absence window and the fence stops the stale one acting.
- No global clock. Per-origin sequences and causal dots only.
- No merge inside Dannan, no mutable values, no namespace, key, path, topic, inode, task or user.
- No query or index beyond the holdings trie; derived indexes are per hosted system and rebuilt from heads.
- No exactly-once delivery and no cross-origin ordering on any channel.
- Non-Byzantine peers only. A lying node is a compromised certificate, cut off by rotation, not detected by protocol.
- Scope, not isolation. Budgets bound a hosted system's work; they do not sandbox it from another's bugs.
- Durability is a ticket. "Accepted locally" is a real state and the caller chooses what to wait for.
- Reachability of data follows placement. If every holder is off, the object is unreachable and the evidence says so; keys-at-home and a home-satisfiable floor are a domain policy the hosted system sets, not a Dannan default.
- GC is partition-safe, not partition-complete. An orphan protected by an unseen branch's claim survives until that branch is observed.
- Homogeneous protocol; whole-cluster rolling upgrades; fresh-genesis state.
- Relay is a correlation point and relay-only paths are slower on bulk.
- Soft cluster of trusted nodes, tens to low hundreds, every node holding the full roster. Not a public DHT, and a many-tenant public service needs a different trust and placement model.

## Open decisions

- Scope check: looked up in the Lugh head at write time (default here) or carried on the credential. Decide before step 4.
- Head-space replica policy: every-node for Macha, AthenaSIP and Lugh is clear; whether a KV namespace or a repo's refs may run at replicas: 3 from the start, or every head space is every-node in v1. Decide before step 2's seam 7.
- Event loop: Boost.Asio as Dannan's I/O base (AthenaSIP already on it) or Macha's own loop with an Asio adapter. Decide before step 2's seam 1, since it sets the executor rule.
- Punch protocol: UDP-first with TCP fallback, or TCP simultaneous-open only. UDP punches far more NATs; Macha's lanes are TCP today.
- Protocol-version policy for the identity bump in step 4: one bump covering certs and level 0 together, or two.
- Providers and budget for the chaos cluster.
- Whether Status is a Dannan noun (shared Status page across hosted systems) or each hosted system keeps its own, with Dannan publishing only its counters.

Sources: /home/claude/macha (tomdionysus/macha, develop, 0.91.0) and /home/claude/athena/athenasip (blackravenltd/athenasip, 0.12.0), both cloned 2026-11-02; the Macha Core Extraction Plan written earlier tonight.
