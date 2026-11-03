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

Bytes label(const char* prefix, std::span<const uint8_t> data) {
  Bytes out(reinterpret_cast<const uint8_t*>(prefix), reinterpret_cast<const uint8_t*>(prefix) + std::strlen(prefix));
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

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
std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>> derive_keys(std::array<uint8_t, 32> shared, const identity::ClusterKey& key, const Bytes& hello,
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
  auto out = std::make_pair(identity::hkdf_sha256(shared, key.auth, c2s.data()), identity::hkdf_sha256(shared, key.auth, s2c.data()));
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

}  // namespace

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

ClientHandshake::ClientHandshake(const identity::ClusterKey& key, membership::NodeRecord local, size_t max_frame, Lane lane, Purpose purpose, PathToken token,
                                 WireVersions versions)
    : _key(key), _local(std::move(local)), _max_frame(max_frame), _lane(lane), _purpose(purpose), _versions(versions) {
  validate_frame_limit(_max_frame);
  validate_versions(_versions);
  if (_purpose == Purpose::ordinary && token != PathToken{}) throw std::invalid_argument("an ordinary connection carries no path token");
  _nonce = random32();
  _ephemeral = identity::x25519_generate();
  Writer hello;
  hello.u16(_versions.min);
  hello.u16(_versions.max);
  hello.u32(static_cast<uint32_t>(_max_frame));
  hello.u8(static_cast<uint8_t>(_lane));
  hello.fixed(_key.cluster_id);
  hello.fixed(_local.id.bytes);
  hello.fixed(_nonce);
  hello.fixed(_ephemeral.public_key);
  hello.u8(static_cast<uint8_t>(_purpose));
  if (_purpose != Purpose::ordinary) hello.fixed(token);
  membership::encode(hello, _local);
  _hello = hello.take();
  Writer envelope;
  envelope.bytes(_hello);
  envelope.fixed(identity::hmac_sha256(_key.auth, label("dannan/hello/1", _hello)));
  _blob = framed(envelope.data());
}

ClientHandshake::~ClientHandshake() { identity::cleanse(_ephemeral.private_key); }

Established ClientHandshake::finish(std::span<const uint8_t> response) {
  return refusing([&] {
    Reader envelope(response);
    const auto ack = envelope.bytes(max_handshake_blob);
    const auto mac = envelope.fixed<32>();
    envelope.finish();
    auto authenticated = label("dannan/ack/1", _hello);
    authenticated.insert(authenticated.end(), ack.begin(), ack.end());
    if (!identity::constant_time_equal(mac, identity::hmac_sha256(_key.auth, authenticated))) throw HandshakeRefused("peer auth failed");

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

    const auto shared = identity::x25519_shared(_ephemeral.private_key, server_public);
    identity::cleanse(_ephemeral.private_key);
    auto [c2s, s2c] = derive_keys(shared, _key, _hello, ack, _local.id, peer.id, _nonce, server_nonce);
    Established out;
    out.peer = std::move(peer);
    out.lane = _lane;
    out.max_frame = negotiated;
    out.version = version;
    out.purpose = _purpose;
    out.observed = std::move(observed);
    out.keys.tx = c2s;
    out.keys.rx = s2c;
    out.keys.session_id = _nonce;
    return out;
  });
}

ServerReply server_handshake(const identity::ClusterKey& key, const membership::NodeRecord& local, size_t max_frame, std::span<const uint8_t> envelope_bytes,
                             const Endpoint& remote, const Expected* expected, WireVersions versions) {
  validate_frame_limit(max_frame);
  validate_versions(versions);
  return refusing([&] {
    Reader envelope(envelope_bytes);
    const auto hello = envelope.bytes(max_handshake_blob);
    const auto mac = envelope.fixed<32>();
    envelope.finish();
    if (!identity::constant_time_equal(mac, identity::hmac_sha256(key.auth, label("dannan/hello/1", hello)))) throw HandshakeRefused("client auth failed");

    Reader reader(hello);
    const auto their_min = reader.u16();
    const auto their_max = reader.u16();
    if (!their_min || their_min > their_max) throw HandshakeRefused("invalid wire version range");
    const auto version = std::min(their_max, versions.max);
    if (version < std::max(their_min, versions.min))
      throw HandshakeRefused("no common wire version: ours " + std::to_string(versions.min) + ".." + std::to_string(versions.max) + ", theirs " +
                             std::to_string(their_min) + ".." + std::to_string(their_max));
    const size_t peer_max = reader.u32();
    check_limit(peer_max);
    const auto lane_byte = reader.u8();
    if (lane_byte < 1 || lane_byte > 3) throw HandshakeRefused("invalid transport lane");
    const auto lane = static_cast<Lane>(lane_byte);
    if (reader.fixed<16>() != key.cluster_id) throw HandshakeRefused("wrong cluster");
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

    auto authenticated = label("dannan/ack/1", hello);
    authenticated.insert(authenticated.end(), ack.begin(), ack.end());
    Writer response;
    response.bytes(ack);
    response.fixed(identity::hmac_sha256(key.auth, authenticated));

    const auto shared = identity::x25519_shared(ephemeral.private_key, client_public);
    identity::cleanse(ephemeral.private_key);
    auto [c2s, s2c] = derive_keys(shared, key, hello, ack, peer.id, local.id, client_nonce, server_nonce);
    ServerReply out;
    out.response_blob = framed(response.data());
    out.session.peer = std::move(peer);
    out.session.lane = lane;
    out.session.max_frame = negotiated;
    out.session.version = version;
    out.session.purpose = purpose;
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
