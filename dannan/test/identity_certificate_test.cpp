//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Certificates and revocations (TODO/2026-11-04-identity.md, sections 1 and
// 3): a cluster CA (EC P-256, self-signed, CA:TRUE pathlen 0), node
// certificates it issues over a node's own key naming the node id in a
// subjectAltName URI, the checks a node certificate must pass, the cluster
// id from the CA's key, and revocations the CA signs.

#include <gtest/gtest.h>

#include "dannan/identity/certificate.h"

namespace dannan::identity {
namespace {

using namespace std::chrono_literals;

constexpr uint64_t now = 1'800'000'000'000;  // 2027-01-15, the tests' clock
constexpr uint64_t day = 24ull * 3600 * 1000;

NodeId node(uint8_t n) {
  NodeId out;
  out.bytes.fill(n);
  return out;
}

TEST(Certificate, TheCaIsSelfSignedAndNamesTheCluster) {
  const auto ca = CertificateAuthority::create("test cluster", now);
  const auto& cert = ca.certificate();
  EXPECT_TRUE(cert.is_ca());
  EXPECT_FALSE(cert.node_id());
  EXPECT_EQ(cert.subject(), "CN=test cluster");
  EXPECT_LE(cert.not_before_unix_ms(), now);
  EXPECT_GT(cert.not_after_unix_ms(), now + 365 * day);
  EXPECT_EQ(cert.public_key(), ca.key().public_key());
  // One CA, one cluster: the id is the CA's, and another CA's differs.
  EXPECT_EQ(cluster_id_of(cert), cluster_id_of(cert));
  EXPECT_NE(cluster_id_of(cert), cluster_id_of(CertificateAuthority::create("test cluster", now).certificate()));
}

TEST(Certificate, ANodeCertificateNamesItsNodeAndKey) {
  const auto ca = CertificateAuthority::create("c", now);
  const auto key = SigningKey::generate(SigningAlgorithm::es256);
  const auto cert = ca.issue(node(7), key.public_key(), 42, now);
  EXPECT_FALSE(cert.is_ca());
  EXPECT_EQ(cert.node_id(), node(7));
  EXPECT_EQ(cert.serial(), 42u);
  EXPECT_EQ(cert.public_key(), key.public_key());
  EXPECT_EQ(cert.not_after_unix_ms() - cert.not_before_unix_ms(), 2 * 365 * day);
  EXPECT_EQ(check_node_certificate(cert, ca.certificate(), now), "");
}

TEST(Certificate, RoundTripsThroughDerAndPem) {
  const auto ca = CertificateAuthority::create("c", now);
  const auto cert = ca.issue(node(1), SigningKey::generate(SigningAlgorithm::es256).public_key(), 9, now);
  EXPECT_EQ(Certificate::from_der(cert.der()).der(), cert.der());
  const auto pem = cert.pem();
  EXPECT_EQ(pem.rfind("-----BEGIN CERTIFICATE-----", 0), 0u);
  EXPECT_EQ(Certificate::from_pem(pem).der(), cert.der());
  EXPECT_EQ(Certificate::from_der(cert.der()).node_id(), node(1));
  EXPECT_THROW(Certificate::from_der(Bytes{1, 2, 3}), std::runtime_error);
  EXPECT_THROW(Certificate::from_pem("not a certificate"), std::runtime_error);
}

// What a node certificate must be: issued by this CA, within its validity
// at the receiver's clock, a leaf, and naming a node.
TEST(Certificate, ANodeCertificateIsCheckedAgainstTheCaAndTheClock) {
  const auto ca = CertificateAuthority::create("c", now);
  const auto other = CertificateAuthority::create("c", now);
  const auto key = SigningKey::generate(SigningAlgorithm::es256).public_key();
  const auto cert = ca.issue(node(3), key, 1, now);
  EXPECT_NE(check_node_certificate(cert, other.certificate(), now), "");
  EXPECT_NE(check_node_certificate(other.issue(node(3), key, 1, now), ca.certificate(), now), "");
  EXPECT_NE(check_node_certificate(cert, ca.certificate(), now - day), "");            // not yet valid
  EXPECT_NE(check_node_certificate(cert, ca.certificate(), now + 3 * 365 * day), "");  // expired
  EXPECT_NE(check_node_certificate(ca.certificate(), ca.certificate(), now), "");      // a CA is no node
  const auto short_lived = ca.issue(node(3), key, 2, now, 24h);
  EXPECT_EQ(check_node_certificate(short_lived, ca.certificate(), now + day / 2), "");
  EXPECT_NE(check_node_certificate(short_lived, ca.certificate(), now + 2 * day), "");
  EXPECT_THROW(ca.issue(node(3), Bytes{1, 2, 3}, 3, now), std::runtime_error);
}

// The CA's key and certificate come back together; a key that is not the
// certificate's is refused.
TEST(Certificate, ACaIsLoadedFromItsKeyAndCertificate) {
  const auto ca = CertificateAuthority::create("c", now);
  const auto loaded = CertificateAuthority(SigningKey::from_pkcs8(ca.key().to_pkcs8().reveal()), Certificate::from_der(ca.certificate().der()));
  const auto cert = loaded.issue(node(4), SigningKey::generate(SigningAlgorithm::es256).public_key(), 5, now);
  EXPECT_EQ(check_node_certificate(cert, ca.certificate(), now), "");
  EXPECT_THROW(CertificateAuthority(SigningKey::generate(SigningAlgorithm::es256), ca.certificate()), std::invalid_argument);
}

// A revocation the CA signs: its signature is checked against the CA, it
// covers the certificate it names (or every certificate of the node with
// serial zero), and it round-trips, alone and as a set.
TEST(Revocation, IsSignedByTheCaAndCoversWhatItNames) {
  const auto ca = CertificateAuthority::create("c", now);
  const auto key = SigningKey::generate(SigningAlgorithm::es256).public_key();
  const auto first = ca.issue(node(5), key, 10, now);
  const auto second = ca.issue(node(5), key, 11, now);
  const auto one = ca.revoke(node(5), 10, now, "lost laptop");
  EXPECT_TRUE(verify_revocation(one, ca.certificate()));
  EXPECT_TRUE(one.covers(first));
  EXPECT_FALSE(one.covers(second));
  const auto all = ca.revoke(node(5), 0, now, "decommissioned");
  EXPECT_TRUE(all.covers(first) && all.covers(second));
  EXPECT_FALSE(all.covers(ca.issue(node(6), key, 12, now)));
  // Another CA's, or one altered, is not this cluster's revocation.
  EXPECT_FALSE(verify_revocation(CertificateAuthority::create("c", now).revoke(node(5), 10, now, "x"), ca.certificate()));
  auto altered = one;
  altered.serial = 11;
  EXPECT_FALSE(verify_revocation(altered, ca.certificate()));
  const auto set = decode_revocations(encode_revocations({one, all}));
  ASSERT_EQ(set.size(), 2u);
  EXPECT_EQ(set[0], one);
  EXPECT_TRUE(verify_revocation(set[1], ca.certificate()));
  auto bad = encode_revocations({one});
  bad[0] = 'X';
  EXPECT_THROW(decode_revocations(bad), net::DecodeError);
}

}  // namespace
}  // namespace dannan::identity
