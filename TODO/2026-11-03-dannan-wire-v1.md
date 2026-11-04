# Dannan wire v1 (draft for Tom's approval)

Status: APPROVED, 2026-11-03 night (Tom: "Continue please"), with section 8's
recommendations: the interop suite goes, the token rides inside the hello.
Sections 1 to 6 are in code (section 6 as amended in its note); 7 is the
compose bed.

Tom, 2026-11-03: Dannan is a new project, not an extension of Macha; its
protocol versions start at 1; Dannan does everything to maintain the PHY.
This draft is therefore two things: Dannan's own wire, freed of protocol
23, and level 0 (reachability) on top of it.

Integers are big-endian, as now. `string` is u32 length then bytes, `bytes`
the same; `id` is the 16-byte node id.

## 1. What changes from today's (protocol 23) wire

Kept, because they are right rather than inherited: the X25519 handshake
with HMAC over the transcript, HKDF session keys, AES-256-GCM frames with
the header as associated data, per-direction counters, two lanes plus
probe, fragmenting, frame classes, request id parity, the open route table
with core and user types, every limit and the reply rule.

Changed:

| Item | Protocol 23 | Dannan v1 |
|---|---|---|
| Version | 23, exact match | a range: hello carries min and max, the server picks the highest common, none fails (section 2) |
| HKDF/MAC labels | `macha/auth/v1`, `client/v15`, `server/v15`, `macha/session/v15`, `macha/cluster-id/v1` | `dannan/auth/1`, `dannan/hello/1`, `dannan/ack/1`, `dannan/session/1`, `dannan/cluster-id/1` |
| Frame magic | `0x4d433133` ("MC13") | `0x444e0001` ("DN", wire 1) |
| Node record | Macha's NodeInfo, store figures inline, a flags byte | section 3: identity, endpoint, domain, capabilities, reachability, and an advertisement map for whatever a hosted system advertises |
| Message numbers | Macha's | section 4: core 1 to 511, user 512 up (Tom, 2026-11-04) |
| Telemetry, roster file, resets, inbound file magics | `MACHTEL3`, `MACHMEM4`, `MACHIDR1`, `MACHINB1` | `DNNTEL01`, `DNNMEM01`, `DNNRST01`, `DNNINB01`; telemetry keeps its tagged layout, field numbers renumbered, store fields gone (hosted fields from 1024) |
| UPnP description | "Macha <id>" | "Dannan <id>" |
| `.macha.lock` | | `.dannan.lock` |
| Placement salts | `macha/data-*/v7` defaults | no default: the caller names its salts |

Removed with protocol 23: the Macha interop suite (`dannan_interop_tests`:
the in-process byte-for-byte cases against the 0.91.2 export and the
two-process cases against Macha's binary), `tools/macha_types.h`, and
`dannan-node`'s Macha script. They proved compatibility we no longer want.
The rule "a test is never deleted" is set aside for them by this decision;
their Dannan-only equivalents stay or move to the compose cluster
(section 7). Macha learns Dannan's wire when it is refactored.

## 2. Handshake v1

Hello (client to server), MACed with the auth key under `dannan/hello/1`:

```
u16 min_version  u16 max_version   (both 1 today)
u32 max_frame    u8 lane
fixed16 cluster_id   id node   fixed32 nonce   fixed32 x25519_public
record self                         (section 3)
```

Ack (server to client), MACed over hello and ack under `dannan/ack/1`:

```
u16 version (chosen)   u32 max_frame (the smaller)   u8 lane
id node   fixed32 nonce   fixed32 x25519_public
record self
string observed_host   u16 observed_port     (new: where the server saw this connection come from)
```

`observed_*` is the client's address as the server's socket saw it: the
received/rport idea. It is how every node learns its candidate addresses
(section 5) without a STUN server. Session keys as now, under
`dannan/session/1`; the session id is the client's nonce.

Rolling upgrades from here on: a node supports a version range; the
server picks the highest common version; a hello with no overlap is
refused. Nothing is negotiated down below a node's own minimum.

## 3. The node record v1

```
id node   string host   u16 port   string failure_domain
u64 seen_unix_ms
id incarnation            the node's process: a new one is a restart (added 2026-11-03 night)
u32 capabilities          bit 0 inbound (dialable at host:port)
                          bit 1 relay   (forwards for others)
u8  path_class            0 public, 1 punchable, 2 relay_only
u16 candidates            then per candidate: string host, u16 port,
                          u8 confirmed (another node reached it), u64 confirmed_unix_ms
u16 adverts               then per advert: string key, bytes value
```

`adverts` is the hosted system's: a store puts its capacity, use and
write floor there; Dannan never reads them. Bounded: 64 adverts, 1 KiB
each, 16 candidates.

Placement reads capacity from an advert the caller names, so Dannan's
placement stays generic.

## 4. Message numbers

Core (Dannan's, 1 to 511; a hosted system's from 512, registered freely):

| # | Name | Lane, class | Payload |
|---|---|---|---|
| 1 | ping | both, control | empty |
| 2 | members | control, control | empty; reply 101 |
| 3 | session_retire | both, control | empty |
| 4 | promote_read_ahead | both, control | u64 request id |
| 5 | promote_foreground | both, control | u64 request id |
| 6 | cancel_transfer | both, control | u64 request id |
| 7 | telemetry | control, any | telemetry set; reply 102 |
| 8 | identity_resets | control, control | reset set; reply 103 |
| 9 | dial_request | control, control, notification | u8 lane |
| 10 | dial_back | control, control | string host, u16 port; reply 104 |
| 11 | punch_request | control, control | id target, u8 lane, string host, u16 port; reply 105 |
| 12 | punch_go | control, control | id peer, u8 lane, fixed16 token, string host, u16 port; reply 105 |
| 13 | relay_open | control, control | section 6; reply 106 |
| 14 | relay_offer | control, control | section 6; reply 107 |
| 15 | relay_data | both, any, notification | u64 circuit, bytes |
| 16 | relay_close | both, control, notification | u64 circuit, string why |
| 100 | ok | both, any | |
| 101 | members_reply | control, control | u32 count, records |
| 102 | telemetry_reply | control, any | telemetry set |
| 103 | identity_resets_reply | control, control | reset set |
| 104 | dial_back_reply | control, control | u8 reachable, string error |
| 105 | punch_reply | control, control | u8 accepted, string why, string host, u16 port, fixed16 token |
| 106 | relay_open_reply | control, control | u64 circuit or 0, string why, fixed16 token |
| 107 | relay_offer_reply | control, control | u8 accepted, string why |
| 127 | error | both, any | string message, u16 status, string code |

## 5. Path classes and candidates

Reachability is per pair; a node's `path_class` is what it knows of
itself:

- **public**: inbound-capable (dial-back confirmed, as now).
- **punchable**: not dialable, but its NAT kept the same external port
  for two different peers (two `observed` answers agree on the port for
  one local port): endpoint-independent mapping, so a simultaneous open
  can work.
- **relay_only**: neither (symmetric NAT or CGNAT that changes ports).

Candidates: every handshake's `observed` address is a candidate; it is
confirmed when a peer reaches it (the dial-back, or a punch that
completes). Gossiped in the record; confirmed ones are tried first.

Per pair, in order, with a deadline each:

1. **Direct**: either side is public; the other dials, or asks with
   dial_request (as now).
2. **Punched**: both punchable (or one punchable, one public behind a
   firewall): brokered simultaneous open (section 6).
3. **Relayed**: through a node with the relay capability both reach.

A path change is a transport reconnect under the same node identity;
in-flight calls retry under the existing rule; lanes keep their class. A
node holding a relayed or punched path re-tries the better classes every
reprobe interval and moves up when one works.

## 6. Punch and relay flows

**As built (2026-11-03 night).** Two changes from the flows below, both
found in writing them. A node does not keep its broker session on a fixed
port (two CONTROL dials from one port to one peer would collide on their
4-tuple); instead each punch starts with a probe-lane handshake from a
fresh punch port to the broker, whose ack's observed address is that
port's mapping. And `punch_go` is a request, not a notification, because
the target must answer with its own mapping: A observes its punch port
and sends `punch_request{B, lane, A's address}`; the broker calls B with
`punch_go{A, lane, token, A's address}`; B observes its own punch port,
answers `punch_reply` with its address and starts connecting to A at
once; the broker answers A with B's address and the token, and A starts
connecting. Both retry for the punch window (3 s); there is no `at` time,
so clocks need not agree. The lower node id runs the dialler's
handshake, the token inside the hello (section 8, item 3). Relayed bytes
travel on the ends' CONTROL sessions with the relay, as control frames
for a CONTROL circuit and loader frames for a DATA one, and are handled
in arrival order per peer.

**Punch.** Each node keeps its CONTROL session to a public peer (the
broker) on a fixed local port `P` (bound with SO_REUSEADDR/REUSEPORT), so
its NAT mapping for `P` is stable and is what the broker observed.

1. A sends `punch_request{B, lane}` to a broker both have sessions with.
2. The broker checks both are punchable, answers A `punch_reply`, and
   sends both `punch_go`:
   `id peer, u16 candidates (host, port), u64 at_unix_ms, fixed16 token, u8 lane`
   with `at` a short time ahead (default 500 ms).
3. At `at`, each connects from local port `P` to the peer's candidates,
   retrying for 3 s. TCP simultaneous open yields one socket per side.
4. The lower node id runs the dialler's handshake, the higher the
   acceptor's. Each checks the `token` (where: section 8, item 3), so a
   stray connection is closed. The session is then an ordinary route.

**Relay.** A circuit carries one end-to-end session's bytes through a
relay R; R sees only ciphertext.

1. A sends R `relay_open{id target B, u8 lane, fixed16 token}`.
2. R sends B `relay_offer{u64 circuit, id from A, u8 lane, fixed16 token}`
   over B's CONTROL session; B answers `relay_offer_reply`.
3. R answers A `relay_open_reply{circuit}`. A and B each attach a local
   socketpair to the circuit and run the normal handshake and Session over
   it (A as dialler); bytes travel as `relay_data{circuit, bytes}` on A to
   R and R to B, in the class of the frame they carry, on the lane the
   circuit is for.
4. Either end, or R, ends it with `relay_close`.

Rules from the design: CONTROL relay is unconditional; DATA relay is
charged to R's loader class and refused past its budget; R publishes
relayed bytes per pair in Status; placement can be told a peer's path
class and prefers direct and punched peers for bulk.

## 7. The compose test bed (Docker, test only)

A Linux image building Dannan and `dannan-node`; networks:

- `public`: two public nodes (one a relay).
- `lan-a` behind a router container with endpoint-independent NAT
  (iptables MASQUERADE): punchable.
- `lan-b` behind a router with port-randomising NAT
  (MASQUERADE --random-fully): relay-only.
- A CGNAT pair (two NAT layers) for relay-only on both sides.

A scenario runner (a small script driving `docker compose` and the nodes'
Status) asserts: every node is a member of one cluster; each pair's path
class is the expected one; calls on both lanes work over every class;
terminating a router moves the affected pairs to the next class and back;
a partition heals. Run on the laptop by hand; never part of the per-commit
gate.

## 8. Decisions asked of Tom

1. Approve this wire, or amend it.
2. The interop suite and Macha type table go with protocol 23 (section 1).
3. The punch token check (section 6, step 4): after the handshake as the
   first notification (as drafted), or inside the hello as a field.
   Recommendation: inside the hello, a `u8 purpose` and `fixed16 token`
   added to the hello for punched and relayed sessions, so a stray
   connection fails the handshake rather than after it.
