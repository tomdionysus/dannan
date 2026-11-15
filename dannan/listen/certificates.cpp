//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/listen/certificates.h"

#include <openssl/crypto.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include "dannan/listen/detail.h"

namespace dannan::listen {

namespace asio = boost::asio;
namespace ssl = boost::asio::ssl;

Certificates::Material::~Material() {
  if (!key.empty()) OPENSSL_cleanse(key.data(), key.size());
}

namespace {

std::string read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read " + path.string());
  std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!in.good() && !in.eof()) throw std::runtime_error("cannot read " + path.string());
  return out;
}

int select_alpn(SSL*, const unsigned char** out, unsigned char* out_length, const unsigned char* in, unsigned int in_length, void* arg) {
  const auto* wire = static_cast<const std::string*>(arg);
  unsigned char* chosen = nullptr;
  unsigned char chosen_length = 0;
  // The server's order decides: the first of its protocols the client offered.
  if (SSL_select_next_proto(&chosen, &chosen_length, reinterpret_cast<const unsigned char*>(wire->data()), static_cast<unsigned>(wire->size()), in,
                            in_length) != OPENSSL_NPN_NEGOTIATED)
    return SSL_TLSEXT_ERR_NOACK;
  *out = chosen;
  *out_length = chosen_length;
  return SSL_TLSEXT_ERR_OK;
}

// Under `optional` the chain's verdict is recorded and the handshake goes on.
int keep_going(int, X509_STORE_CTX*) { return 1; }

// Builds and checks a load under a default policy: the chain, the key that is its
// leaf's, the client bundle. Each failure names the path, never what was in it.
void check(const Certificates::Material& material, const CertificatePaths& paths) {
  ssl::context context(ssl::context::tls_server);
  boost::system::error_code error;
  context.use_certificate_chain(asio::buffer(material.chain), error);
  if (error) throw std::runtime_error(paths.chain.string() + ": not a PEM certificate chain");
  context.use_private_key(asio::buffer(material.key), ssl::context::pem, error);
  if (error) throw std::runtime_error(paths.key.string() + ": not a PEM private key");
  if (SSL_CTX_check_private_key(context.native_handle()) != 1) throw std::runtime_error(paths.key.string() + ": not the key of " + paths.chain.string());
  if (!paths.client_ca.empty()) {
    context.add_certificate_authority(asio::buffer(material.client_ca), error);
    if (error) throw std::runtime_error(paths.client_ca.string() + ": not a PEM certificate bundle");
  }
}

}  // namespace

Certificates::Certificates(CertificatePaths paths) : _paths(std::move(paths)) { reload(); }

Certificates::~Certificates() = default;

void Certificates::reload() {
  auto material = std::make_shared<Material>();
  material->chain = read(_paths.chain);
  material->key = read(_paths.key);
  if (!_paths.client_ca.empty()) material->client_ca = read(_paths.client_ca);
  check(*material, _paths);
  contract::Lock lock(_mutex);
  _material = std::move(material);
  ++_generation;
}

std::shared_ptr<const Certificates::Material> Certificates::current() const {
  contract::Lock lock(_mutex);
  return _material;
}

uint64_t Certificates::generation() const {
  contract::Lock lock(_mutex);
  return _generation;
}

namespace detail {

std::shared_ptr<ServerTls> server_tls(const Certificates::Material& material, const TlsPolicy& policy, const std::string& listener) {
  auto out = std::make_shared<ServerTls>(ssl::context::tls_server);
  auto& context = out->context;
  SSL_CTX* native = context.native_handle();
  context.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1 |
                      ssl::context::single_dh_use);
  if (SSL_CTX_set_min_proto_version(native, policy.minimum == TlsVersion::tls13 ? TLS1_3_VERSION : TLS1_2_VERSION) != 1)
    throw std::runtime_error(listener + ": cannot set the minimum TLS version");
  boost::system::error_code error;
  context.use_certificate_chain(asio::buffer(material.chain), error);
  if (!error) context.use_private_key(asio::buffer(material.key), ssl::context::pem, error);
  if (error) throw std::runtime_error(listener + ": cannot use its certificate");
  out->client_bundle = !material.client_ca.empty();
  if (out->client_bundle) {
    context.add_certificate_authority(asio::buffer(material.client_ca), error);
    if (error) throw std::runtime_error(listener + ": cannot use its client bundle");
  }
  // Verifying peers needs a session id context, or a resumed session is refused.
  static const unsigned char session_context[] = "dannan-listen";
  SSL_CTX_set_session_id_context(native, session_context, sizeof(session_context) - 1);
  switch (policy.client_certificates) {
    case ClientCertificates::never:
      SSL_CTX_set_verify(native, SSL_VERIFY_NONE, nullptr);
      break;
    case ClientCertificates::optional:
      SSL_CTX_set_verify(native, SSL_VERIFY_PEER, keep_going);
      break;
    case ClientCertificates::required:
      SSL_CTX_set_verify(native, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
      break;
  }
  if (!policy.alpn.empty()) {
    for (const auto& protocol : policy.alpn) {
      if (protocol.empty() || protocol.size() > 255) throw std::runtime_error(listener + ": an ALPN protocol name is 1 to 255 bytes");
      out->alpn_wire.push_back(static_cast<char>(protocol.size()));
      out->alpn_wire += protocol;
    }
    SSL_CTX_set_alpn_select_cb(native, select_alpn, &out->alpn_wire);
  }
  return out;
}

void when_time_reaches(const std::shared_ptr<asio::steady_timer>& timer, const contract::TimeSource& time, contract::TimePoint at, std::function<void()> fire) {
  const auto remaining = std::max(std::chrono::ceil<std::chrono::milliseconds>(at - time.now()), std::chrono::milliseconds(0));
  timer->expires_after(time.wake_within(remaining));
  timer->async_wait([timer, &time, at, fire = std::move(fire)](const boost::system::error_code& error) mutable {
    if (error) return;
    if (time.now() >= at) return fire();
    when_time_reaches(timer, time, at, std::move(fire));
  });
}

}  // namespace detail

}  // namespace dannan::listen
