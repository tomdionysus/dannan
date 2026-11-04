//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/channel.h"

#include <algorithm>
#include <cstring>

namespace dannan::net {
namespace {

// "DN", wire 1.
constexpr uint32_t frame_magic = 0x444e0001;
constexpr uint8_t flag_first = 0x01;
constexpr uint8_t flag_last = 0x02;

Bytes framed(const Bytes& body) {
  Writer writer;
  writer.u32(static_cast<uint32_t>(body.size()));
  writer.raw(body);
  return writer.take();
}

Bytes session_info(std::span<const uint8_t> transcript, const identity::NodeId& client, const identity::NodeId& server, const char* direction) {
  Writer writer;
  writer.string("dannan/session/1");
  writer.string(direction);
  writer.fixed(identity::sha256(transcript).bytes);
  writer.fixed(client.bytes);
  writer.fixed(server.bytes);
  return writer.take();
}

// Both directions' keys, bound to the transcript, both ids and both nonces.
// The client sends with c2s; the server with s2c.
std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>> derive_keys(std::array<uint8_t, 32> shared, std::span<const uint8_t> salt, const Bytes& hello,
                                                                        const Bytes& ack, const identity::NodeId& client, const identity::NodeId& server,
                                                                        const std::array<uint8_t, 32>& client_nonce,
                                                                        const std::array<uint8_t, 32>& server_nonce) {
  Writer transcript_writer;
  transcript_writer.bytes(hello);
  transcript_writer.bytes(ack);
  const auto transcript = transcript_writer.take();
  Writer c2s;
  c2s.raw(session_info(transcript, client, server, "c2s"));
  c2s.fixed(client_nonce);
  c2s.fixed(server_nonce);
  Writer s2c;
  s2c.raw(session_info(transcript, client, server, "s2c"));
  s2c.fixed(client_nonce);
  s2c.fixed(server_nonce);
  auto out = std::make_pair(identity::hkdf_sha256(shared, salt, c2s.data()), identity::hkdf_sha256(shared, salt, s2c.data()));
  identity::cleanse(shared);
  return out;
}

std::array<uint8_t, 32> random32() {
  const auto bytes = identity::random_bytes(32);
  std::array<uint8_t, 32> out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

void check_limit(size_t size) {
  if (size < min_frame_size || size > max_frame_size) throw HandshakeRefused("max frame size must be 4K..4M");
}

void validate_versions(WireVersions versions) {
  if (!versions.min || versions.min > versions.max) throw std::invalid_argument("a wire version range is min..max from 1");
}

// Decode errors in a handshake are refusals, not crashes.
template <class F>
auto refusing(F&& f) {
  try {
    return f();
  } catch (const DecodeError& error) {
    throw HandshakeRefused(std::string("malformed handshake: ") + error.what());
  }
}

constexpr uint8_t proof_by_key = 1;
constexpr uint8_t proof_by_certificate = 2;

Bytes concat(std::initializer_list<std::span<const uint8_t>> parts) {
  Bytes out;
  for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
  return out;
}

std::span<const uint8_t> text(const char* s) { return {reinterpret_cast<const uint8_t*>(s), std::strlen(s)}; }

std::array<uint8_t, 16> key_cluster(const identity::ClusterKey& key) { return key.cluster_id; }

// A peer's certificate, checked: this cluster's CA, the receiver's clock,
// naming `node`, not revoked.
identity::Certificate checked_certificate(const CertificateCredential& mine, std::span<const uint8_t> der, const identity::NodeId& node) {
  identity::Certificate certificate = [&] {
    try {
      return identity::Certificate::from_der(der);
    } catch (const std::runtime_error& error) {
      throw HandshakeRefused(std::string("not a certificate: ") + error.what());
    }
  }();
  if (const auto why = identity::check_node_certificate(certificate, mine.ca, mine.wall->unix_ms()); !why.empty()) throw HandshakeRefused(why);
  if (certificate.node_id() != node) throw HandshakeRefused("the certificate names another node");
  if (mine.revoked && mine.revoked(certificate)) throw HandshakeRefused("the certificate is revoked");
  return certificate;
}

void check_signature(const identity::Certificate& certificate, std::span<const uint8_t> message, std::span<const uint8_t> signature) {
  const auto key = certificate.public_key();
  const auto algorithm = identity::algorithm_of_public_key(key);
  if (!algorithm || !identity::verify(*algorithm, key, message, signature)) throw HandshakeRefused("the signature does not verify");
}

}  // namespace

WireVersions Credentials::versions() const {
  if (!key && !certificate) throw std::logic_error("credentials hold neither a cluster key nor a certificate");
  return {static_cast<uint16_t>(key ? 1 : 2), static_cast<uint16_t>(certificate ? 2 : 1)};
}

const char* purpose_name(Purpose purpose) noexcept {
  switch (purpose) {
    case Purpose::ordinary:
      return "ordinary";
    case Purpose::punched:
      return "punched";
    case Purpose::relayed:
      return "relayed";
  }
  return "unknown";
}

void validate_frame_limit(size_t size) {
  if (size < min_frame_size || size > max_frame_size) throw std::invalid_argument("max frame size must be 4K..4M");
}

ClientHandshake::ClientHandshake(Credentials credentials, membership::NodeRecord local, size_t max_frame, Lane lane, Purpose purpose, PathToken token,
                                 std::optional<WireVersions> versions)
    : _credentials(std::move(credentials)), _local(std::move(local)), _max_frame(max_frame), _lane(lane), _purpose(purpose) {
  validate_frame_limit(_max_frame);
  _versions = versions ? *versions : _credentials.versions();
  validate_versions(_versions);
  if (_purpose == Purpose::ordinary && token != PathToken{}) throw std::invalid_argument("an ordinary connection carries no path token");
  const bool by_key = _credentials.key && _versions.min <= 1;
  const bool by_certificate = _credentials.certificate && _versions.max >= 2;
  if (!by_key && !by_certificate) throw std::invalid_argument("the credentials prove no version in the range");
  _nonce = random32();
  _ephemeral = identity::x25519_generate();
  Writer hello;
  hello.u16(_versions.min);
  hello.u16(_versions.max);
  hello.u32(static_cast<uint32_t>(_max_frame));
  hello.u8(static_cast<uint8_t>(_lane));
  hello.fixed(_local.id.bytes);
  hello.fixed(_nonce);
  hello.fixed(_ephemeral.public_key);
  hello.u8(static_cast<uint8_t>(_purpose));
  if (_purpose != Purpose::ordinary) hello.fixed(token);
  membership::encode(hello, _local);
  _hello = hello.take();
  Writer envelope;
  envelope.bytes(_hello);
  envelope.u8(static_cast<uint8_t>((by_key ? 1 : 0) + (by_certificate ? 1 : 0)));
  if (by_key) {
    const auto& key = *_credentials.key;
    const auto cluster = key_cluster(key);
    envelope.u8(proof_by_key);
    envelope.fixed(cluster);
    envelope.fixed(identity::hmac_sha256(key.auth, concat({text("dannan/hello/1"), _hello, cluster})));
  }
  if (by_certificate) {
    const auto& mine = *_credentials.certificate;
    const auto cluster = identity::cluster_id_of(mine.ca);
    envelope.u8(proof_by_certificate);
    envelope.fixed(cluster);
    envelope.bytes(mine.own.der());
    envelope.bytes(mine.key.sign(concat({text("dannan/hello/2"), _hello, cluster})));
  }
  _blob = framed(envelope.data());
}

ClientHandshake::~ClientHandshake() { identity::cleanse(_ephemeral.private_key); }

Established ClientHandshake::finish(std::span<const uint8_t> response) {
  return refusing([&] {
    Reader envelope(response);
    const auto ack = envelope.bytes(max_handshake_blob);
    Reader reader(ack);
    const auto version = reader.u16();
    if (version < _versions.min || version > _versions.max) throw HandshakeRefused("peer chose a wire version outside our range");
    const size_t negotiated = reader.u32();
    check_limit(negotiated);
    if (static_cast<Lane>(reader.u8()) != _lane) throw HandshakeRefused("peer acknowledged the wrong transport lane");
    if (negotiated > _max_frame) throw HandshakeRefused("peer negotiated an invalid frame size");
    identity::NodeId server_id{reader.fixed<16>()};
    const auto server_nonce = reader.fixed<32>();
    const auto server_public = reader.fixed<32>();
    auto peer = membership::decode(reader);
    Endpoint observed;
    observed.host = reader.string(4096);
    observed.port = reader.u16();
    reader.finish();
    if (peer.id != server_id) throw HandshakeRefused("peer identity mismatch");

    std::optional<identity::Certificate> peer_certificate;
    Bytes salt;
    if (version == 1) {
      if (!_credentials.key) throw HandshakeRefused("peer chose version 1 without a cluster key here");
      const auto mac = envelope.fixed<32>();
      envelope.finish();
      if (!identity::constant_time_equal(mac, identity::hmac_sha256(_credentials.key->auth, concat({text("dannan/ack/1"), _hello, ack}))))
        throw HandshakeRefused("peer auth failed");
      salt.assign(_credentials.key->auth.begin(), _credentials.key->auth.end());
    } else {
      if (!_credentials.certificate) throw HandshakeRefused("peer chose version 2 without a certificate here");
      const auto der = envelope.bytes(max_handshake_blob);
      const auto signature = envelope.bytes(512);
      envelope.finish();
      peer_certificate = checked_certificate(*_credentials.certificate, der, server_id);
      check_signature(*peer_certificate, concat({text("dannan/ack/2"), _hello, ack}), signature);
      const auto cluster = identity::cluster_id_of(_credentials.certificate->ca);
      salt.assign(cluster.begin(), cluster.end());
    }

    const auto shared = identity::x25519_shared(_ephemeral.private_key, server_public);
    identity::cleanse(_ephemeral.private_key);
    auto [c2s, s2c] = derive_keys(shared, salt, _hello, ack, _local.id, peer.id, _nonce, server_nonce);
    Established out;
    out.peer = std::move(peer);
    out.lane = _lane;
    out.max_frame = negotiated;
    out.version = version;
    out.purpose = _purpose;
    out.observed = std::move(observed);
    out.peer_certificate = std::move(peer_certificate);
    out.keys.tx = c2s;
    out.keys.rx = s2c;
    out.keys.session_id = _nonce;
    return out;
  });
}

ServerReply server_handshake(const Credentials& credentials, const membership::NodeRecord& local, size_t max_frame, std::span<const uint8_t> envelope_bytes,
                             const Endpoint& remote, const Expected* expected, std::optional<WireVersions> versions_given) {
  validate_frame_limit(max_frame);
  const auto versions = versions_given ? *versions_given : credentials.versions();
  validate_versions(versions);
  return refusing([&] {
    Reader envelope(envelope_bytes);
    const auto hello = envelope.bytes(max_handshake_blob);
    // The proofs, by kind; each names the cluster it proves.
    struct Proof {
      std::array<uint8_t, 16> cluster{};
      std::array<uint8_t, 32> mac{};
      Bytes der;
      Bytes signature;
    };
    std::optional<Proof> by_key, by_certificate;
    const auto proofs = envelope.u8();
    if (proofs > 2) throw HandshakeRefused("too many proofs");
    for (uint8_t i = 0; i < proofs; ++i) {
      const auto kind = envelope.u8();
      Proof proof;
      proof.cluster = envelope.fixed<16>();
      if (kind == proof_by_key) {
        proof.mac = envelope.fixed<32>();
        by_key = std::move(proof);
      } else if (kind == proof_by_certificate) {
        proof.der = envelope.bytes(max_handshake_blob);
        proof.signature = envelope.bytes(512);
        by_certificate = std::move(proof);
      } else {
        throw HandshakeRefused("unknown proof");
      }
    }
    envelope.finish();

    Reader reader(hello);
    const auto their_min = reader.u16();
    const auto their_max = reader.u16();
    if (!their_min || their_min > their_max) throw HandshakeRefused("invalid wire version range");
    const size_t peer_max = reader.u32();
    check_limit(peer_max);
    const auto lane_byte = reader.u8();
    if (lane_byte < 1 || lane_byte > 3) throw HandshakeRefused("invalid transport lane");
    const auto lane = static_cast<Lane>(lane_byte);
    identity::NodeId client_id{reader.fixed<16>()};
    const auto client_nonce = reader.fixed<32>();
    const auto client_public = reader.fixed<32>();
    const auto purpose_byte = reader.u8();
    if (purpose_byte > static_cast<uint8_t>(Purpose::relayed)) throw HandshakeRefused("invalid connection purpose");
    const auto purpose = static_cast<Purpose>(purpose_byte);
    PathToken token{};
    if (purpose != Purpose::ordinary) token = reader.fixed<16>();
    auto peer = membership::decode(reader);
    reader.finish();
    if (peer.id != client_id) throw HandshakeRefused("client identity mismatch");

    // The highest version both speak that the hello proves for this cluster.
    const auto top = std::min(their_max, versions.max);
    const auto bottom = std::max(their_min, versions.min);
    if (top < bottom)
      throw HandshakeRefused("no common wire version: ours " + std::to_string(versions.min) + ".." + std::to_string(versions.max) + ", theirs " +
                             std::to_string(their_min) + ".." + std::to_string(their_max));
    uint16_t version = 0;
    std::optional<identity::Certificate> peer_certificate;
    std::string refused = "the hello proves no common version for this cluster";
    for (uint16_t v = top; v >= bottom && !version; --v) {
      if (v == 2 && credentials.certificate && by_certificate) {
        const auto& mine = *credentials.certificate;
        if (by_certificate->cluster != identity::cluster_id_of(mine.ca)) {
          refused = "wrong cluster";
        } else {
          peer_certificate = checked_certificate(mine, by_certificate->der, client_id);
          check_signature(*peer_certificate, concat({text("dannan/hello/2"), hello, by_certificate->cluster}), by_certificate->signature);
          version = 2;
        }
      } else if (v == 1 && credentials.key && by_key) {
        const auto& key = *credentials.key;
        if (by_key->cluster != key_cluster(key)) {
          refused = "wrong cluster";
        } else if (!identity::constant_time_equal(by_key->mac, identity::hmac_sha256(key.auth, concat({text("dannan/hello/1"), hello, by_key->cluster})))) {
          throw HandshakeRefused("client auth failed");
        } else {
          version = 1;
        }
      }
      if (v == 1) break;
    }
    if (!version) throw HandshakeRefused(refused);
    if (!expected) {
      if (purpose != Purpose::ordinary) throw HandshakeRefused(std::string("a ") + purpose_name(purpose) + " connection nobody expected");
    } else if (purpose != expected->purpose || !identity::constant_time_equal(token, expected->token) || client_id != expected->peer) {
      throw HandshakeRefused(std::string("not the expected ") + purpose_name(expected->purpose) + " connection");
    }
    if (peer.host.empty() || peer.host == "0.0.0.0" || peer.host == "::") peer.host = remote.host;

    const auto negotiated = std::min(peer_max, max_frame);
    const auto server_nonce = random32();
    auto ephemeral = identity::x25519_generate();
    Writer ack_writer;
    ack_writer.u16(version);
    ack_writer.u32(static_cast<uint32_t>(negotiated));
    ack_writer.u8(lane_byte);
    ack_writer.fixed(local.id.bytes);
    ack_writer.fixed(server_nonce);
    ack_writer.fixed(ephemeral.public_key);
    membership::encode(ack_writer, local);
    ack_writer.string(remote.host);
    ack_writer.u16(remote.port);
    const auto ack = ack_writer.take();

    Writer response;
    response.bytes(ack);
    Bytes salt;
    if (version == 1) {
      response.fixed(identity::hmac_sha256(credentials.key->auth, concat({text("dannan/ack/1"), hello, ack})));
      salt.assign(credentials.key->auth.begin(), credentials.key->auth.end());
    } else {
      const auto& mine = *credentials.certificate;
      response.bytes(mine.own.der());
      response.bytes(mine.key.sign(concat({text("dannan/ack/2"), hello, ack})));
      const auto cluster = identity::cluster_id_of(mine.ca);
      salt.assign(cluster.begin(), cluster.end());
    }

    const auto shared = identity::x25519_shared(ephemeral.private_key, client_public);
    identity::cleanse(ephemeral.private_key);
    auto [c2s, s2c] = derive_keys(shared, salt, hello, ack, peer.id, local.id, client_nonce, server_nonce);
    ServerReply out;
    out.response_blob = framed(response.data());
    out.session.peer = std::move(peer);
    out.session.lane = lane;
    out.session.max_frame = negotiated;
    out.session.version = version;
    out.session.purpose = purpose;
    out.session.peer_certificate = std::move(peer_certificate);
    out.session.keys.tx = s2c;
    out.session.keys.rx = c2s;
    out.session.keys.session_id = client_nonce;
    return out;
  });
}

FrameSealer::FrameSealer(const Established& session, std::shared_ptr<const RouteTable> routes)
    : _key(session.keys.tx), _lane(session.lane), _max_frame(session.max_frame), _routes(std::move(routes)) {
  if (!_routes) throw std::invalid_argument("FrameSealer: a route table is required");
}

FrameSealer::~FrameSealer() { identity::cleanse(_key); }

Bytes FrameSealer::seal(uint64_t request_id, sched::FrameType frame, MessageType type, bool first, bool last, std::span<const uint8_t> payload) {
  if (payload.size() > _max_frame) throw std::invalid_argument("frame payload exceeds the negotiated limit");
  if (!_routes->allowed(type, _lane, frame))
    throw std::invalid_argument("message " + _routes->name(type) + " may not travel as " + sched::frame_type_name(frame) + " on the " + lane_name(_lane) +
                                " lane");
  Writer header;
  header.u32(frame_magic);
  header.u8(static_cast<uint8_t>(frame));
  header.u8(static_cast<uint8_t>((first ? flag_first : 0) | (last ? flag_last : 0)));
  header.u16(static_cast<uint16_t>(type));
  header.u32(static_cast<uint32_t>(payload.size()));
  header.u64(++_counter);
  header.u64(request_id);
  auto out = header.take();
  const auto sealed = _nonces ? identity::aes_gcm_seal(_key, _nonces(), payload, out) : identity::aes_gcm_seal(_key, payload, out);
  out.reserve(out.size() + 12 + 16 + sealed.ciphertext.size());
  out.insert(out.end(), sealed.nonce.begin(), sealed.nonce.end());
  out.insert(out.end(), sealed.tag.begin(), sealed.tag.end());
  out.insert(out.end(), sealed.ciphertext.begin(), sealed.ciphertext.end());
  return out;
}

FrameOpener::FrameOpener(const Established& session, std::shared_ptr<const RouteTable> routes)
    : _key(session.keys.rx), _lane(session.lane), _max_frame(session.max_frame), _routes(std::move(routes)) {
  if (!_routes) throw std::invalid_argument("FrameOpener: a route table is required");
}

FrameOpener::~FrameOpener() { identity::cleanse(_key); }

size_t FrameOpener::header(std::span<const uint8_t, frame_header_size> bytes) {
  Reader reader(bytes);
  if (reader.u32() != frame_magic) throw FrameRefused("bad frame magic");
  const auto frame = sched::decode_frame_type(reader.u8());
  if (!frame) throw FrameRefused("bad frame type");
  const auto flags = reader.u8();
  if (flags & ~(flag_first | flag_last)) throw FrameRefused("bad frame flags");
  const MessageType type{reader.u16()};
  const auto size = reader.u32();
  const auto counter = reader.u64();
  const auto request_id = reader.u64();
  if (size > _max_frame || counter != _counter + 1) throw FrameRefused("bad frame sequence");
  if (!_routes->allowed(type, _lane, *frame))
    throw FrameRefused("message " + _routes->name(type) + " is invalid as " + sched::frame_type_name(*frame) + " on the " + lane_name(_lane) + " lane");
  std::copy(bytes.begin(), bytes.end(), _header.begin());
  _pending = WireFragment{request_id, *frame, type, (flags & flag_first) != 0, (flags & flag_last) != 0, {}};
  _pending_size = size;
  _have_header = true;
  return 12 + 16 + size;
}

WireFragment FrameOpener::open(std::span<const uint8_t> body) {
  if (!_have_header) throw FrameRefused("frame body without a header");
  _have_header = false;
  if (body.size() != 12 + 16 + _pending_size) throw FrameRefused("frame body has the wrong length");
  try {
    _pending.payload = identity::aes_gcm_open(_key, std::span<const uint8_t, 12>(body.data(), 12), std::span<const uint8_t, 16>(body.data() + 12, 16),
                                              body.subspan(28), _header);
  } catch (const std::runtime_error&) {
    throw FrameRefused("frame failed authentication");
  }
  ++_counter;
  return std::move(_pending);
}

}  // namespace dannan::net
