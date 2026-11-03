//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/codec.h"
#include "dannan/net/routes.h"
#include "dannan/sched/work.h"

// The authenticated channel, without I/O: protocol 23's handshake (v15) and
// its sealed frames as functions over bytes. The transport moves the bytes;
// everything that decides whether they are right is here, where it can be
// tested alone and against Macha's own channel.
//
// Handshake. The client sends a hello: the protocol version, its frame
// limit, the lane, the cluster id, its node id, a fresh nonce, an ephemeral
// X25519 key and its node record, MACed with the cluster's auth key under
// "client/v15". The server answers with an ack of the same shape (the frame
// limit is the smaller of the two), MACed under "server/v15" over hello and
// ack. Each side derives its send and receive keys by HKDF over the X25519
// secret, salted with the auth key, bound to the transcript, both node ids
// and both nonces. Any other protocol version fails the handshake; nothing
// is negotiated down. The session id is the client's nonce.
//
// Frames. A 28-byte header (magic, frame type, flags, message type, payload
// size, counter, request id), a 12-byte nonce, a 16-byte tag, then the
// payload under AES-256-GCM with the header as associated data. Each
// direction counts its frames from 1; a frame out of sequence is refused.
namespace dannan::net {

inline constexpr uint16_t protocol_version = 23;
inline constexpr size_t min_frame_size = 4 * 1024;
inline constexpr size_t max_frame_size = 4 * 1024 * 1024;
inline constexpr size_t max_handshake_blob = 16384;
inline constexpr size_t frame_header_size = 28;
inline constexpr size_t frame_overhead = frame_header_size + 12 + 16;

// A handshake that did not authenticate or did not agree; the session ends.
class HandshakeRefused : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A frame that is malformed, out of sequence, off its lane or class, or
// fails authentication; the session ends.
class FrameRefused : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct SessionKeys {
  std::array<uint8_t, 32> tx{};
  std::array<uint8_t, 32> rx{};
  std::array<uint8_t, 32> session_id{};
};

// A completed handshake, from one end.
struct Established {
  membership::NodeRecord peer;
  Lane lane = Lane::control;
  size_t max_frame = 0;
  SessionKeys keys;
};

// Throws std::invalid_argument unless 4 KiB <= size <= 4 MiB.
void validate_frame_limit(size_t size);

// The client's half. Construct, send hello_blob(), read the server's blob
// (its u32 length checked against max_handshake_blob by the transport), and
// pass its body to finish(). Single owner.
class ClientHandshake {
 public:
  // Throws std::invalid_argument for a frame limit outside 4K..4M.
  ClientHandshake(const identity::ClusterKey& key, membership::NodeRecord local, size_t max_frame, Lane lane);
  ~ClientHandshake();
  ClientHandshake(const ClientHandshake&) = delete;
  ClientHandshake& operator=(const ClientHandshake&) = delete;

  // The hello as it goes on the wire, its u32 length first.
  const Bytes& hello_blob() const noexcept { return _blob; }
  // Throws HandshakeRefused for an ack that does not authenticate or agree.
  Established finish(std::span<const uint8_t> response);

 private:
  identity::ClusterKey _key;
  membership::NodeRecord _local;
  size_t _max_frame;
  Lane _lane;
  std::array<uint8_t, 32> _nonce{};
  identity::X25519KeyPair _ephemeral;
  Bytes _hello;
  Bytes _blob;
};

struct ServerReply {
  // The ack as it goes on the wire, its u32 length first.
  Bytes response_blob;
  Established session;
};

// The server's half, from the body of the client's blob. `remote_host` is
// the address the connection came from, used when the client advertises
// none. Throws HandshakeRefused for a hello that does not authenticate or
// agree, std::invalid_argument for a frame limit outside 4K..4M.
ServerReply server_handshake(const identity::ClusterKey& key, const membership::NodeRecord& local, size_t max_frame, std::span<const uint8_t> hello,
                             const std::string& remote_host);

struct WireFragment {
  uint64_t request_id = 0;
  sched::FrameType frame = sched::FrameType::control;
  MessageType type{};
  bool first = false;
  bool last = false;
  Bytes payload;
};

// Seals one direction's frames. Single owner: the session's writer.
class FrameSealer {
 public:
  // The table is the node's, shared by every session.
  FrameSealer(const Established& session, std::shared_ptr<const RouteTable> routes);
  ~FrameSealer();
  // The frame as it goes on the wire. Throws std::invalid_argument for a
  // payload over the negotiated limit, or a type the table forbids on this
  // lane in this class.
  Bytes seal(uint64_t request_id, sched::FrameType frame, MessageType type, bool first, bool last, std::span<const uint8_t> payload);
  uint64_t counter() const noexcept { return _counter; }
  // Tests only: frames are sealed under the nonces `nonces` gives instead of
  // fresh random ones, so a frame can be compared byte for byte with
  // another implementation's.
  void use_nonces_for_test(std::function<std::array<uint8_t, 12>()> nonces) { _nonces = std::move(nonces); }

 private:
  std::array<uint8_t, 32> _key;
  Lane _lane;
  size_t _max_frame;
  std::shared_ptr<const RouteTable> _routes;
  uint64_t _counter = 0;
  std::function<std::array<uint8_t, 12>()> _nonces;
};

// Opens the other direction's frames, header first, so a bad frame is
// refused before its body is read. Single owner: the session's reader.
class FrameOpener {
 public:
  FrameOpener(const Established& session, std::shared_ptr<const RouteTable> routes);
  ~FrameOpener();
  // Checks a header and returns the bytes that follow it (nonce, tag and
  // payload). Throws FrameRefused.
  size_t header(std::span<const uint8_t, frame_header_size> header);
  // Opens the body of the frame whose header was just accepted. Throws
  // FrameRefused if it does not authenticate.
  WireFragment open(std::span<const uint8_t> body);
  uint64_t counter() const noexcept { return _counter; }

 private:
  std::array<uint8_t, 32> _key;
  Lane _lane;
  size_t _max_frame;
  std::shared_ptr<const RouteTable> _routes;
  uint64_t _counter = 0;
  std::array<uint8_t, frame_header_size> _header{};
  WireFragment _pending;
  size_t _pending_size = 0;
  bool _have_header = false;
};

}  // namespace dannan::net
