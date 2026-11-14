# P3: a shared public listener (design, 2026-11-14)

the maintainer's ruling (2026-11-13): "build the shared public listener in Dannan, below the
protocols." The requirements are Lugh's and Lir's combined, in
`2026-11-13-stack-needs.md` P3 (ten items, numbered R1 to R10 here). This note is
the shape proposed, for review by Lugh and Lir (and AthenaSIP's needs, which are likely
the same) before any code. Nothing here is built.

## The line

Dannan owns what every public protocol shares: accept, TLS and DTLS, StartTLS, limits,
deadlines, a bounded close, stop and binding. Parsing HTTP, LDAP, MQTT, MQTT-SN,
WebSocket and SIP stays each hosted system's own (the stack's "Decided" 9 holds: there
is no shared HTTPS stack). The cluster transport (`net::`) is untouched: public clients
never meet the cluster CA or the cluster key (R2).

## Proposal

A new subsystem, `dannan::listen`, built by a hosted system on the node's executor;
the node owns none, so a hosted system with no public face pays nothing.

1. **`listen::Certificates`**: a listener's PEM chain and key (R2), loaded into an
   OpenSSL context. `reload()` builds a new context and swaps it for the next
   handshake; connections already open keep theirs. A reload that fails keeps the old
   context and says why. Reload is called by the hosted system (on SIGHUP, say);
   Dannan does not watch files.
2. **`listen::TlsPolicy`**: per listener (R3, R4): the minimum version (1.2 or 1.3),
   client certificates `never`, `optional` or `required` with the PEM bundle that
   verifies them, and ALPN. A browser-facing listener sets `never`.
3. **`listen::StreamListener`**: TCP, optionally TLS, optionally StartTLS-capable.
   Each accepted connection is handed up as a `listen::Stream` on the caller's
   executor or strand (R1), with the peer's address and, for a verified client
   certificate, its subject and SHA-256 thumbprint (RFC 8705). `Stream` reads and
   writes asynchronously, upgrades in place with `start_tls()` once the hosted system
   has read the request, refusing any plaintext byte after it (R5), and closes either
   cleanly (bounded `close_notify`, default 2 s, as Lir's) or at once (R8).
4. **Deadlines** per listener on injected time (R8): the handshake's, the idle one,
   and an outbound byte bound past which a slow reader is closed.
5. **`listen::Limits`**, shared across a hosted system's listeners (R7):
   - an accept rate and a connection cap per listener;
   - a per-address bucket consulted before any credential;
   - a per-caller limit the hosted system charges once it knows who is calling, and
     which the listener consults at accept for an address it has tied to a caller.
   A refusal is counted by reason and closes at once.
6. **`listen::forwarded_for()`** (R6): the client address from `X-Forwarded-For`
   given the trusted proxies and the peer, a helper the HTTP code applies. Dannan does
   not parse HTTP.
7. **Binding** (R10): `listen::inherited(name)` takes a socket-activation fd by name
   (`LISTEN_FDS`, `LISTEN_FDNAMES`); without one, the listener binds itself, which on
   a privileged port needs `CAP_NET_BIND_SERVICE`.
8. **Stop** (R9): a listener's `stop()` closes its acceptor and every connection it
   handed up, cleanly within the close bound, then at once.
9. **`listen::DatagramListener`**: UDP, optionally DTLS (R1), handing up an
   `Association` per peer, with DTLS cookies against address spoofing. Lir has a
   hand-written DTLS server today (`lir/sn/gateway.cpp`); Dannan's is written from
   this design and Lir's behaviour, read for what it does.

Counters per listener (accepted, refused by reason, handshake failures, closes by kind,
open now) go on the hosted system's `status::Surface` under the listener's name.

## Phasing

The stream side (pieces 1 to 8) is the larger share of Lugh's and Lir's use and is
well trodden in Asio. DTLS (piece 9) is not in Asio, and it is a protocol state machine
of its own. Proposal: the stream side in 0.8.0, DTLS in 0.9.0, so Lir keeps its own
DTLS server until then.

## Questions

For Lugh and Lir:
1. Does the line hold: everything above the stream and the association yours?
2. Is a reload you call (on SIGHUP) enough, or must Dannan watch the files?
3. The per-caller limit: is "an object you charge, consulted at accept by address" the
   shape you meant, or should the listener call back into you at accept?
4. Lir: does the stream side in 0.8.0 and DTLS in 0.9.0 suit you, keeping your own
   DTLS server until then?

For the maintainer:
1. The subsystem's name, `dannan::listen` (beside `net`, `membership`, `reach`)?
   Recommendation: yes; `net` is the cluster's transport, and keeping the public side
   apart keeps the cluster CA out of it.
2. The phasing: stream side in 0.8.0, DTLS in 0.9.0? Recommendation: yes.
