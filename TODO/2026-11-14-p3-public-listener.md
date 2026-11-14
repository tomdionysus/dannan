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
   handshake; connections already open keep theirs. A reload covers the client-CA
   bundle too. A reload that fails keeps the old context and names the path that failed,
   never a value read from it (Lugh's review). Reload is called by the hosted system (on
   SIGHUP, from the host's renewal hook); Dannan does not watch files. One
   `Certificates` may serve several listeners, and one reload swaps all of them (Lir's
   review: its MQTT TLS, WSS and HTTPS share a certificate).
2. **`listen::TlsPolicy`**: per listener (R3, R4): the minimum version (1.2 or 1.3),
   client certificates `never`, `optional` or `required` with the PEM bundle that
   verifies them, and ALPN (Lugh's HTTPS: `http/1.1` only). A browser-facing listener
   sets `never`. Under `optional` every connection is asked for a certificate, and the
   chain check never ends the handshake: its verdict is kept on the connection for the
   hosted system to judge (Lugh's review: RFC 8705's certificate-bound tokens bind to
   any certificate, self-signed included). Under `required` a chain that does not
   verify ends the handshake.
3. **`listen::StreamListener`**: TCP, optionally TLS, optionally StartTLS-capable.
   Each accepted connection is handed up as a `listen::Stream` on the caller's
   executor or strand (R1), with the peer's address and, for any client certificate
   presented, its SHA-256 thumbprint (RFC 8705), its subject, and whether its chain
   verified against the bundle, with the reason when it did not. `Stream` models Asio's
   `AsyncReadStream` and `AsyncWriteStream` (`async_read_some`, `async_write_some`,
   `get_executor`), so Beast's HTTP and WebSocket run over it as over any socket, and it
   gives the TLS session (`SSL*`) and the transport's name (Lir's review). It upgrades
   in place with `start_tls()` once the hosted system
   has read the request, refusing any plaintext byte after it (R5), and closes either
   cleanly (bounded `close_notify`, default 2 s, as Lir's) or at once (R8).
4. **Deadlines** per listener on injected time (R8): the handshake's; the idle one,
   off by default and settable on each Stream after accept (Lir's review: MQTT keep
   alive is per client, and SSE streams are long-lived with nothing inbound); and an
   outbound byte bound, settable per listener (Lir's is 4 MiB), on writes the Stream
   queues itself, so a hosted system may send one write per packet without waiting and
   a slow reader is closed once its queued bytes pass the bound.
5. **`listen::Limits`**, shared across a hosted system's listeners (R7). Every limit
   is optional and off by default (Lir's review: past its own cap Lir answers CONNACK
   0x89 so a client is told why, and a socket closed at accept makes clients redial at
   once; Lir sets Dannan's cap only as a backstop above its own):
   - an accept rate and a connection cap per listener;
   - a per-address bucket consulted before any credential. Trusted proxy peers are
     exempt, and a listener may turn the bucket off (Lugh's review: behind a reverse
     proxy the TCP peer is the proxy, so the bucket would limit every client as one;
     the hosted system limits forwarded_for()'s answer per request instead);
   - optionally, a per-caller limit the hosted system charges once it knows who is
     calling, consulted at accept for an address it has tied to a caller. For a
     protocol with one identity per connection (Lir's MQTT); a protocol that learns
     the caller per request (Lugh's HTTP, over keep-alive and NAT) leaves it out and
     limits per request itself. No callback into the hosted system at accept.
   A refusal is counted by reason and closes at once.
6. **`listen::forwarded_for()`** (R6): the client address from `X-Forwarded-For`
   given the trusted proxies and the peer, a helper the HTTP code applies. Dannan does
   not parse HTTP.
7. **Binding** (R10): `listen::inherited(name)` takes a socket-activation fd by name
   (`LISTEN_FDS`, `LISTEN_FDNAMES`); without one, the listener binds itself, which on
   a privileged port needs `CAP_NET_BIND_SERVICE`.
8. **Stop** (R9), in two steps (Lir's review): `stop_accepting()` closes the acceptor;
   then, after a window the hosted system chooses (in which Lir sends MQTT 5's DISCONNECT
   0x8B to its clients), `close_all()` closes every connection it handed up, cleanly
   within the close bound, then at once. `stop()` is the two with no window.
9. **`listen::DatagramListener`**: UDP, optionally DTLS (R1), handing up an
   `Association` per peer, plain UDP as well as DTLS (MQTT-SN runs over both), with DTLS
   cookies against address spoofing, the client certificate on DTLS, and a broadcast
   send from the same socket (the MQTT-SN gateway's ADVERTISE). Lir has a
   hand-written DTLS server today (`lir/sn/gateway.cpp`); Dannan's is written from
   this design and Lir's behaviour, read for what it does.

Counters per listener (accepted, refused by reason, handshake failures, closes by kind,
open now) go on the hosted system's `status::Surface` under the listener's name.

## Phasing

The stream side (pieces 1 to 8) is the larger share of Lugh's and Lir's use and is
well trodden in Asio. DTLS (piece 9) is not in Asio, and it is a protocol state machine
of its own. Proposal: the stream side in 0.8.0, DTLS in 0.9.0, so Lir keeps its own
DTLS server until then.

## Reviews (2026-11-14)

- **Lugh**: the line holds (Beast HTTP, LDAP's BER, routing and request limits stay
  Lugh's); a reload it calls on SIGHUP is enough. Asked for the client-CA bundle in the
  reload and paths, not values, in its failure; any presented certificate's thumbprint,
  subject and chain verdict under `optional`, the handshake never ended by the chain;
  no per-caller callback at accept; trusted proxies exempt from the per-address bucket.
  TLS 1.2 stays allowed on its LDAP for old clients, so the minimum is per listener.
  Lugh moves its HTTPS, mTLS and LDAP listeners onto this after 0.8.0, in its own step.
- **Lir**: the shape suits it, the line holds, a reload it calls is enough, and the
  phasing suits (it keeps `lir/sn/gateway.cpp` until 0.9.0). Asked for: `Stream` as an
  Asio stream with the `SSL*`, for Beast's WebSocket and HTTP; one `Certificates` for
  several listeners; every limit optional and off by default; the idle deadline off by
  default and set per Stream; a per-listener outbound bound on queued writes; a stop in
  two steps; and on the datagram side, plain UDP, DTLS cookies, client certificates and
  broadcast. Lir does not use StartTLS.

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
