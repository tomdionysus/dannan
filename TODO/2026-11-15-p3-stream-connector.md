# The stream connector: dannan::listen's outbound side (0.8 patch)

Asked by Lir, 2026-11-15, for its MQTT client library (`lir::client`, for Louhi), Lugh's
principal-events publisher (Lugh hand-rolls the same today in `lugh/client/https.cpp`) and
Mimir. the maintainer, 2026-11-15, directly: yes, as a 0.8 patch. Rule 3: one copy, the lowest
project. For review by Lir and Lugh before it is built.

## What it is

Connecting out over TCP or TLS, handing up the same `listen::Stream` an accepted
connection is, so a protocol's framing, `send()` with its bound, the draining `close()`,
`when_finished`, `abort`, Beast's view of the read side, `peer()` and `is_tls()` are one
code for both ends. Connecting only: reconnect policy and backoff are the caller's protocol
state.

```cpp
// The server's certificate is checked against `trust`, and its name against
// `server_name` (also sent as SNI). A client certificate is offered when one is given.
struct ClientTls {
  // A PEM bundle of the CAs trusted; empty: the system's store.
  std::filesystem::path trust;
  // The name the server's certificate must carry; empty: the host connected to.
  std::string server_name;
  // The client's own chain and key (CertificatePaths::client_ca unused), reloadable as a
  // listener's are; none: no client certificate.
  std::shared_ptr<Certificates> certificate;
  TlsVersion minimum = TlsVersion::tls12;
  // Offered in order; the server's choice is Stream::alpn().
  std::vector<std::string> alpn;
};

struct ConnectorConfig {
  // Names the connector in its counters and its streams (Stream::listener()).
  std::string name;
  std::chrono::milliseconds handshake_timeout{10000};
  std::chrono::milliseconds close_bound{2000};
  size_t outbound_bound = 4u << 20;
  bool strand_per_stream = true;
};

struct ConnectorCounters { attempts, connected, refused_resolve, refused_connect,
                           handshakes_failed, timed_out, closed_clean, closed_abrupt,
                           slow_readers, open; };

class StreamConnector {
 public:
  StreamConnector(boost::asio::io_context& io, ConnectorConfig config,
                  const contract::TimeSource* time = &contract::steady_time_source());
  ~StreamConnector();  // close_all(), never waits
  // Resolves, connects (every address in turn), sets TCP_NODELAY, and with `tls` takes
  // the handshake, all within `deadline` on the injected time. `done` is posted to
  // `executor` once: the stream, on its own strand of it (or on it as given), or none
  // and why, naming the host and the step, never a value read. From any thread.
  void connect(boost::asio::any_io_executor executor, std::string host, uint16_t port,
               std::optional<ClientTls> tls, std::chrono::milliseconds deadline,
               std::function<void(std::shared_ptr<Stream>, std::string)> done);
  void close_all();
  void close_all(boost::asio::any_io_executor executor, std::function<void()> done);
  ConnectorCounters counters() const;
};
```

`Stream::client_certificate()` on an outbound stream holds the server's certificate,
with `verified` true (a chain or name that fails ends the handshake, so it never reaches
the caller). `start_tls()` on an outbound stream upgrades as the client (a protocol's
STARTTLS), with the `ClientTls` given here: an addition to `connect`'s shape, `tls` with
`upgrade_later = true`, if Lir or Lugh needs it; otherwise left out of this patch.

## How it stays a patch

Within 0.8 no type's layout may change (`tools/ci.sh abi` against 0.8.0):
- The connector's shared state is a `ListenerShared` built from a `ListenerConfig` filled
  from the `ConnectorConfig` (its acceptor never opened), so `Stream` serves both ends
  unchanged: its `_shared` is the same type.
- The TLS context is `detail::ServerTls`'s (it holds an `ssl::context` of any method),
  built by a new `detail::client_tls()`; the name is internal.
- `Stream` gains a private member function for the client handshake and the client
  side's `start_tls`, and `friend class StreamConnector`: new symbols and a friend, no
  layout.
- `ConnectorCounters` are kept in `ListenerShared::counters` (a `ListenerCounters`) and
  mapped on the way out: `accepted` counts connections made, `handshakes_failed`
  handshakes, `refused_*` the refusals, so no counter field is added to a 0.8 type.
- New public types (`ClientTls`, `ConnectorConfig`, `ConnectorCounters`,
  `StreamConnector`) are additions.

## Tests, first

- Plaintext out to a local acceptor: the Stream reads and writes as a socket, the peer is
  the server, TCP_NODELAY is on.
- TLS out to a `StreamListener` with the test PKI: trusted by a bundle; a server name that
  does not match fails the handshake, as does an untrusted chain; ALPN chosen; a client
  certificate offered reaches the listener's `client_certificate()` as verified.
- Resolution fails, nothing listens, and a server that never answers the handshake: each
  told once, with its reason, within the deadline on stepped time.
- `close()` drains `send()` then close_notify, as an accepted stream; `close_all(done)`.
- A connector destroyed with connections in flight: their `done` told once, never after.

## Review

Lir, 2026-11-15:
1. A posted `done(stream, why)` with a deadline: right; no completion token.
2. Client-side STARTTLS: later (MQTT has none). Left out of this patch.
3. The system store as default trust: yes.
Lir's requests, taken:
- a. A constructor taking an executor (`any_io_executor`), as `lir::client` does, beside the
  `io_context&` one; the io context found from it by type, as `stop_accepting` does. An
  executor not of an io context (a thread pool's) is refused, `std::invalid_argument`.
- b. An IP-literal host or `server_name` is checked against the certificate's IP SANs, not
  its DNS names, and no SNI is sent (RFC 6066, section 3). Tested with 127.0.0.1 and a
  certificate carrying IP:127.0.0.1, and one without it refused.
- c. Stated: with `strand_per_stream = false` the stream lives on `connect()`'s executor as
  given, so a client passing its own strand shares it with its timers and callbacks.

Lugh, 2026-11-15 (against `lugh/client/https.cpp`'s checks), taken:
- IP literals as Lir's b.
- Names matched with `X509_VERIFY_PARAM_set1_host` and `X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS`,
  not Asio's `host_name_verification` (deprecated from Boost 1.87); an IP literal with
  `X509_VERIFY_PARAM_set1_ip_asc`. A wildcard matches exactly one label, the leftmost, and
  never a partial label (`*.example.com` matches `a.example.com`, not `a.b.example.com` or
  `example.com`).
- Trust is read once, not per connection: a `ClientTrust` made from a bundle path (or the
  system's default paths when none), throwing at construction and naming the path, with
  `reload()` and a generation as `Certificates` has, and holding its `X509_STORE` in memory;
  `ClientTls` carries it by `shared_ptr`, so a bad bundle is found at start and the
  handshake does no file I/O. So `ClientTls` is
  `{std::shared_ptr<const ClientTrust> trust; std::string server_name;
  std::shared_ptr<Certificates> certificate; TlsVersion minimum = tls12;
  std::vector<std::string> alpn;}`, the trust required.
- Stated: no option turns verification off, and there is no OCSP or CRL check.
- `Stream::peer_certificate()`: the other end's certificate at either end (an accepted
  stream's client certificate, a connected stream's server certificate), a new member;
  `client_certificate()` stays, for an accepted stream.
- Resolution cannot be cancelled (getaddrinfo): the deadline posts `done` once ("did not
  resolve within the deadline") while the resolver may still run, and its late result is
  dropped. Tested with a resolver that never answers, through a seam in the test.
- Answers: posted `done` on the given executor; STARTTLS later; the system store as default.
  Later, perhaps: the cluster's own CA from Dannan's identity as a third trust source.
