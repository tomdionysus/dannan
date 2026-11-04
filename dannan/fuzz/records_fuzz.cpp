//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The records peers gossip: the node record, a members list, telemetry sets,
// identity resets, a channel's interest advert. Each decoder may throw only
// net::DecodeError.

#include "dannan/fuzz/fuzz.h"
#include "dannan/identity/certificate.h"
#include "dannan/membership/identity_reset.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/fanout.h"
#include "dannan/status/telemetry.h"

namespace dannan::fuzz {
namespace {

membership::NodeRecord sample() {
  membership::NodeRecord node;
  node.id = identity::NodeId::random();
  node.host = "198.51.100.7";
  node.port = 7437;
  node.failure_domain = "home";
  node.seen_unix_ms = 1;
  node.candidates = {{"203.0.113.9", 40000, true, 2}};
  node.adverts["capacity"] = membership::u64_advert(1 << 30);
  return node;
}

const Registration node_record(
    "node_record",
    [](std::span<const uint8_t> data) {
      try {
        net::Reader reader(data);
        (void)membership::decode(reader);
      } catch (const net::DecodeError&) {
      }
    },
    [] {
      net::Writer writer;
      membership::encode(writer, sample());
      return std::vector<net::Bytes>{writer.take()};
    });

const Registration telemetry_set(
    "telemetry_set",
    [](std::span<const uint8_t> data) {
      try {
        for (const auto& record : status::decode_telemetry_set(data)) {
          (void)record.traffic();
          (void)record.phase();
        }
      } catch (const net::DecodeError&) {
      }
    },
    [] {
      status::Telemetry record;
      record.set_bytes(status::field::node_id, identity::NodeId::random().bytes);
      record.set_u64(status::field::sequence, 1);
      record.set_text(status::field::host, "h");
      record.set_phase(status::Phase::ready);
      record.set_traffic(1000, {{1, 2, 3, 4, 5}});
      return std::vector<net::Bytes>{status::encode_telemetry_set({record})};
    });

const Registration identity_resets(
    "identity_resets",
    [](std::span<const uint8_t> data) {
      try {
        (void)membership::decode_identity_resets(data);
      } catch (const net::DecodeError&) {
      }
    },
    [] {
      membership::IdentityReset reset;
      reset.host = "h";
      reset.port = 1;
      reset.epoch = 2;
      reset.reason = "r";
      return std::vector<net::Bytes>{membership::encode_identity_resets({reset})};
    });

// A certificate from a peer's hello, and a revocation set from gossip.
// Parsing throws std::runtime_error, and checks run on what parses.
const Registration certificate(
    "certificate",
    [](std::span<const uint8_t> data) {
      static const auto ca = identity::CertificateAuthority::create("fuzz", 1'800'000'000'000);
      try {
        const auto cert = identity::Certificate::from_der(data);
        (void)cert.node_id();
        (void)cert.subject();
        (void)identity::check_node_certificate(cert, ca.certificate(), 1'800'000'000'000);
      } catch (const std::runtime_error&) {
      }
    },
    [] {
      const auto ca = identity::CertificateAuthority::create("fuzz", 1'800'000'000'000);
      identity::NodeId node;
      node.bytes.fill(1);
      return std::vector<net::Bytes>{
          ca.certificate().der(), ca.issue(node, identity::SigningKey::generate(identity::SigningAlgorithm::es256).public_key(), 7, 1'800'000'000'000).der()};
    });

const Registration revocations(
    "revocations",
    [](std::span<const uint8_t> data) {
      static const auto ca = identity::CertificateAuthority::create("fuzz", 1'800'000'000'000);
      try {
        for (const auto& revocation : identity::decode_revocations(data)) (void)identity::verify_revocation(revocation, ca.certificate());
      } catch (const net::DecodeError&) {
      }
    },
    [] {
      const auto ca = identity::CertificateAuthority::create("fuzz", 1'800'000'000'000);
      identity::NodeId node;
      node.bytes.fill(2);
      return std::vector<net::Bytes>{identity::encode_revocations({ca.revoke(node, 3, 1'800'000'000'000, "fuzz")})};
    });

// Never throws: a malformed advert subscribes to nothing.
const Registration interest(
    "channel_interest", [](std::span<const uint8_t> data) { (void)net::decode_interest(data); },
    [] { return std::vector<net::Bytes>{net::encode_interest({"a/", "b/c/"})}; });

}  // namespace
}  // namespace dannan::fuzz
