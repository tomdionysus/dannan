//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/certificate.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <stdexcept>

#include "dannan/identity/crypto.h"

namespace dannan::identity {
namespace {

using X509Pointer = std::unique_ptr<X509, decltype(&X509_free)>;
using Pkey = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

constexpr std::array<uint8_t, 8> revocation_magic{'D', 'N', 'N', 'R', 'E', 'V', '0', '1'};
constexpr std::string_view revocation_label = "dannan/revocation/1";
constexpr std::string_view node_uri_prefix = "dannan:node:";
constexpr uint32_t max_revocations = 65536;

std::shared_ptr<X509> shared(X509* x509) {
  if (!x509) throw std::runtime_error("not a certificate");
  return std::shared_ptr<X509>(x509, X509_free);
}

uint64_t unix_ms_of(const ASN1_TIME* time) {
  std::tm tm{};
  if (!time || ASN1_TIME_to_tm(time, &tm) != 1) throw std::runtime_error("a certificate time that cannot be read");
  return static_cast<uint64_t>(timegm(&tm)) * 1000;
}

void set_time(ASN1_TIME* time, uint64_t unix_ms) {
  if (!ASN1_TIME_set(time, static_cast<time_t>(unix_ms / 1000))) throw std::runtime_error("cannot set a certificate time");
}

void add_extension(X509* x509, X509V3_CTX* context, int nid, const char* value) {
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, context, nid, value);
  if (!extension) throw std::runtime_error(std::string("cannot make certificate extension ") + OBJ_nid2sn(nid));
  const int added = X509_add_ext(x509, extension, -1);
  X509_EXTENSION_free(extension);
  if (added != 1) throw std::runtime_error(std::string("cannot add certificate extension ") + OBJ_nid2sn(nid));
}

void set_serial(X509* x509, uint64_t serial) {
  if (ASN1_INTEGER_set_uint64(X509_get_serialNumber(x509), serial) != 1) throw std::runtime_error("cannot set a certificate serial");
}

void sign(X509* x509, const SigningKey& key) {
  const EVP_MD* digest = key.algorithm() == SigningAlgorithm::es256 ? EVP_sha256() : nullptr;
  if (X509_sign(x509, key.native_handle(), digest) <= 0) throw std::runtime_error("cannot sign a certificate");
}

Bytes unsigned_revocation(const Revocation& revocation) {
  net::Writer writer;
  writer.raw(std::span(reinterpret_cast<const uint8_t*>(revocation_label.data()), revocation_label.size()));
  writer.fixed(revocation.node.bytes);
  writer.u64(revocation.serial);
  writer.u64(revocation.revoked_unix_ms);
  writer.string(revocation.reason);
  return writer.take();
}

}  // namespace

Certificate Certificate::from_der(std::span<const uint8_t> der) {
  const unsigned char* cursor = der.data();
  X509* x509 = d2i_X509(nullptr, &cursor, static_cast<long>(der.size()));
  if (x509 && cursor != der.data() + der.size()) {
    X509_free(x509);
    throw std::runtime_error("trailing bytes after a certificate");
  }
  return Certificate(shared(x509));
}

Certificate Certificate::from_pem(std::string_view pem) {
  Bio bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  if (!bio) throw std::runtime_error("cannot read a certificate");
  return Certificate(shared(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)));
}

Bytes Certificate::der() const {
  const int size = i2d_X509(_x509.get(), nullptr);
  if (size <= 0) throw std::runtime_error("cannot encode a certificate");
  Bytes out(static_cast<size_t>(size));
  unsigned char* cursor = out.data();
  if (i2d_X509(_x509.get(), &cursor) != size) throw std::runtime_error("cannot encode a certificate");
  return out;
}

std::string Certificate::pem() const {
  Bio bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio || PEM_write_bio_X509(bio.get(), _x509.get()) != 1) throw std::runtime_error("cannot encode a certificate");
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, static_cast<size_t>(size));
}

Bytes Certificate::public_key() const {
  EVP_PKEY* key = X509_get0_pubkey(_x509.get());
  const int size = key ? i2d_PUBKEY(key, nullptr) : 0;
  if (size <= 0) throw std::runtime_error("a certificate without a public key");
  Bytes out(static_cast<size_t>(size));
  unsigned char* cursor = out.data();
  if (i2d_PUBKEY(key, &cursor) != size) throw std::runtime_error("cannot encode a public key");
  return out;
}

std::optional<NodeId> Certificate::node_id() const {
  auto* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(_x509.get(), NID_subject_alt_name, nullptr, nullptr));
  if (!names) return std::nullopt;
  std::optional<NodeId> out;
  for (int i = 0; i < sk_GENERAL_NAME_num(names) && !out; ++i) {
    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
    if (name->type != GEN_URI) continue;
    const auto* uri = name->d.uniformResourceIdentifier;
    const std::string text(reinterpret_cast<const char*>(ASN1_STRING_get0_data(uri)), static_cast<size_t>(ASN1_STRING_length(uri)));
    if (text.rfind(node_uri_prefix, 0) == 0) out = NodeId::parse(text.substr(node_uri_prefix.size()));
  }
  GENERAL_NAMES_free(names);
  return out;
}

uint64_t Certificate::serial() const {
  uint64_t out = 0;
  if (ASN1_INTEGER_get_uint64(&out, X509_get0_serialNumber(_x509.get())) != 1) throw std::runtime_error("a certificate serial past 64 bits");
  return out;
}

uint64_t Certificate::not_before_unix_ms() const { return unix_ms_of(X509_get0_notBefore(_x509.get())); }
uint64_t Certificate::not_after_unix_ms() const { return unix_ms_of(X509_get0_notAfter(_x509.get())); }
bool Certificate::is_ca() const { return X509_check_ca(_x509.get()) > 0; }

std::string Certificate::subject() const {
  Bio bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio || X509_NAME_print_ex(bio.get(), X509_get_subject_name(_x509.get()), 0, XN_FLAG_RFC2253) < 0) return {};
  char* data = nullptr;
  const long size = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, static_cast<size_t>(size));
}

Hash256 Certificate::fingerprint() const { return sha256(der()); }

std::array<uint8_t, 16> cluster_id_of(const Certificate& ca) {
  const auto hash = sha256(ca.public_key());
  std::array<uint8_t, 16> out{};
  std::copy_n(hash.bytes.begin(), out.size(), out.begin());
  return out;
}

std::string check_node_certificate(const Certificate& node, const Certificate& ca, uint64_t now_unix_ms) {
  if (node.is_ca()) return "a CA's certificate is no node's";
  if (!node.node_id()) return "the certificate names no node";
  if (X509_check_issued(ca.native_handle(), node.native_handle()) != X509_V_OK || X509_verify(node.native_handle(), X509_get0_pubkey(ca.native_handle())) != 1)
    return "the certificate was not issued by this cluster's CA";
  try {
    if (now_unix_ms < node.not_before_unix_ms()) return "the certificate is not valid yet";
    if (now_unix_ms > node.not_after_unix_ms()) return "the certificate has expired";
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

bool Revocation::covers(const Certificate& certificate) const {
  const auto named = certificate.node_id();
  return named && *named == node && (serial == 0 || serial == certificate.serial());
}

bool verify_revocation(const Revocation& revocation, const Certificate& ca) noexcept {
  try {
    const auto key = ca.public_key();
    const auto algorithm = algorithm_of_public_key(key);
    return algorithm && verify(*algorithm, key, unsigned_revocation(revocation), revocation.signature);
  } catch (const std::exception&) {
    return false;
  }
}

Bytes encode_revocations(const std::vector<Revocation>& revocations) {
  net::Writer writer;
  writer.raw(revocation_magic);
  writer.u32(static_cast<uint32_t>(revocations.size()));
  for (const auto& revocation : revocations) {
    writer.fixed(revocation.node.bytes);
    writer.u64(revocation.serial);
    writer.u64(revocation.revoked_unix_ms);
    writer.string(revocation.reason);
    writer.bytes(revocation.signature);
  }
  return writer.take();
}

std::vector<Revocation> decode_revocations(std::span<const uint8_t> bytes) {
  net::Reader reader(bytes);
  if (reader.fixed<8>() != revocation_magic) throw net::DecodeError("not a revocation set");
  const auto count = reader.u32();
  if (count > max_revocations) throw net::DecodeError("too many revocations");
  std::vector<Revocation> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Revocation revocation;
    revocation.node.bytes = reader.fixed<16>();
    revocation.serial = reader.u64();
    revocation.revoked_unix_ms = reader.u64();
    revocation.reason = reader.string(4096);
    revocation.signature = reader.bytes(512);
    out.push_back(std::move(revocation));
  }
  reader.finish();
  return out;
}

CertificateAuthority CertificateAuthority::create(const std::string& name, uint64_t now_unix_ms, std::chrono::hours validity) {
  return create(name, now_unix_ms, SigningKey::generate(SigningAlgorithm::es256), validity);
}

CertificateAuthority CertificateAuthority::create(const std::string& name, uint64_t now_unix_ms, SigningKey key, std::chrono::hours validity) {
  X509Pointer x509(X509_new(), X509_free);
  if (!x509 || X509_set_version(x509.get(), 2) != 1) throw std::runtime_error("cannot make a certificate");
  set_serial(x509.get(), 1);
  set_time(X509_getm_notBefore(x509.get()), now_unix_ms);
  set_time(X509_getm_notAfter(x509.get()), now_unix_ms + static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(validity).count()));
  X509_NAME* subject = X509_get_subject_name(x509.get());
  if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0) != 1 ||
      X509_set_issuer_name(x509.get(), subject) != 1 || X509_set_pubkey(x509.get(), key.native_handle()) != 1)
    throw std::runtime_error("cannot make a CA certificate");
  X509V3_CTX context;
  X509V3_set_ctx(&context, x509.get(), x509.get(), nullptr, nullptr, 0);
  add_extension(x509.get(), &context, NID_basic_constraints, "critical,CA:TRUE,pathlen:0");
  add_extension(x509.get(), &context, NID_key_usage, "critical,keyCertSign,cRLSign");
  add_extension(x509.get(), &context, NID_subject_key_identifier, "hash");
  sign(x509.get(), key);
  return CertificateAuthority(std::move(key), Certificate(shared(x509.release())));
}

CertificateAuthority::CertificateAuthority(SigningKey key, Certificate certificate) : _key(std::move(key)), _certificate(std::move(certificate)) {
  if (!_certificate.is_ca()) throw std::invalid_argument("not a CA's certificate");
  if (_key.public_key() != _certificate.public_key()) throw std::invalid_argument("the key is not the CA certificate's");
}

Certificate CertificateAuthority::issue(const NodeId& node, std::span<const uint8_t> public_key, uint64_t serial, uint64_t now_unix_ms,
                                        std::chrono::hours validity) const {
  const unsigned char* cursor = public_key.data();
  Pkey subject_key(d2i_PUBKEY(nullptr, &cursor, static_cast<long>(public_key.size())), EVP_PKEY_free);
  if (!subject_key || cursor != public_key.data() + public_key.size() || !algorithm_of_public_key(public_key))
    throw std::runtime_error("not an ES256 or Ed25519 public key");
  X509Pointer x509(X509_new(), X509_free);
  if (!x509 || X509_set_version(x509.get(), 2) != 1) throw std::runtime_error("cannot make a certificate");
  set_serial(x509.get(), serial);
  set_time(X509_getm_notBefore(x509.get()), now_unix_ms);
  set_time(X509_getm_notAfter(x509.get()), now_unix_ms + static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(validity).count()));
  const auto id = to_string(node);
  X509_NAME* subject = X509_get_subject_name(x509.get());
  if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(id.c_str()), -1, -1, 0) != 1 ||
      X509_set_issuer_name(x509.get(), X509_get_subject_name(_certificate.native_handle())) != 1 || X509_set_pubkey(x509.get(), subject_key.get()) != 1)
    throw std::runtime_error("cannot make a node certificate");
  X509V3_CTX context;
  X509V3_set_ctx(&context, _certificate.native_handle(), x509.get(), nullptr, nullptr, 0);
  add_extension(x509.get(), &context, NID_basic_constraints, "critical,CA:FALSE");
  add_extension(x509.get(), &context, NID_key_usage, "critical,digitalSignature");
  add_extension(x509.get(), &context, NID_subject_alt_name, ("URI:" + std::string(node_uri_prefix) + id).c_str());
  add_extension(x509.get(), &context, NID_authority_key_identifier, "keyid");
  sign(x509.get(), _key);
  return Certificate(shared(x509.release()));
}

Revocation CertificateAuthority::revoke(const NodeId& node, uint64_t serial, uint64_t now_unix_ms, std::string reason) const {
  Revocation out{node, serial, now_unix_ms, std::move(reason), {}};
  out.signature = _key.sign(unsigned_revocation(out));
  return out;
}

}  // namespace dannan::identity
