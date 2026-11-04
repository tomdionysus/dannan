//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-ca: a cluster's certificate authority, on an operator's machine,
// never on a node (TODO/2026-11-04-identity.md, section 4). The CA's key
// is held in key custody under the CA's directory, sealed under a systemd
// credential or a passphrase, never on disk in clear; the CA's certificate
// is <dir>/ca.pem.
//
//   dannan-ca init   --dir DIR --name NAME KEY [--days N]
//       makes the CA; under a credential, prints the new root (hex) for
//       `systemd-creds encrypt`
//   dannan-ca issue  --dir DIR KEY --node ID --public-key HEX [--serial N] [--days N]
//       prints a node certificate (PEM) over the node's key, as
//       `dannan-node --show-identity` printed them
//   dannan-ca revoke --dir DIR KEY --node ID [--serial N] --reason TEXT --out FILE
//       writes a revocation (serial 0 or none: every certificate of the
//       node), for `dannan-node --revocations FILE` on any node
//   dannan-ca show   FILE
//       what a certificate (PEM) or a revocation file says
//
// KEY is --passphrase-env VAR (the passphrase in that environment
// variable) or --credential NAME [--credentials-dir DIR].

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>

#include "dannan/identity/certificate.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"
#include "dannan/identity/key_custody.h"

namespace {

using namespace dannan;

uint64_t now_ms() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

std::map<std::string, std::string> parse(int argc, char** argv, int from) {
  std::map<std::string, std::string> out;
  for (int i = from; i < argc; ++i) {
    const std::string option = argv[i];
    if (option.rfind("--", 0) != 0 || i + 1 >= argc) throw std::runtime_error("expected --option value, at " + option);
    out[option.substr(2)] = argv[++i];
  }
  return out;
}

std::string need(const std::map<std::string, std::string>& options, const std::string& name) {
  const auto found = options.find(name);
  if (found == options.end()) throw std::runtime_error("--" + name + " is required");
  return found->second;
}

identity::KeySource key_source(const std::map<std::string, std::string>& options) {
  if (const auto env = options.find("passphrase-env"); env != options.end()) {
    const char* value = std::getenv(env->second.c_str());
    if (!value || !*value) throw std::runtime_error("the passphrase variable " + env->second + " is empty");
    const std::string passphrase(value);
    return identity::KeySource::typed_passphrase(
        [passphrase] { return identity::Secret(std::span(reinterpret_cast<const uint8_t*>(passphrase.data()), passphrase.size())); });
  }
  if (const auto credential = options.find("credential"); credential != options.end()) {
    const auto directory = options.contains("credentials-dir") ? std::filesystem::path(options.at("credentials-dir")) : std::filesystem::path();
    return identity::KeySource::systemd_credential(credential->second, directory);
  }
  throw std::runtime_error("the CA key's source: --passphrase-env VAR or --credential NAME");
}

identity::Certificate read_certificate(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  const std::string pem((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return identity::Certificate::from_pem(pem);
}

identity::CertificateAuthority open_ca(const std::map<std::string, std::string>& options) {
  const std::filesystem::path dir = need(options, "dir");
  auto custody = identity::KeyCustody::open(dir, key_source(options));
  return identity::CertificateAuthority(custody.node_key(), read_certificate(dir / "ca.pem"));
}

identity::NodeId node_of(const std::map<std::string, std::string>& options) {
  const auto id = identity::NodeId::parse(need(options, "node"));
  if (!id) throw std::runtime_error("--node is a node id, 32 hex digits");
  return *id;
}

uint64_t serial_of(const std::map<std::string, std::string>& options) {
  if (const auto given = options.find("serial"); given != options.end()) return std::stoull(given->second);
  // Random, 63 bits: positive in X.509, and never two alike in practice.
  const auto bytes = identity::random_bytes(8);
  uint64_t out = 0;
  for (const auto byte : bytes) out = (out << 8) | byte;
  return (out >> 1) | 1;
}

std::chrono::hours days(const std::map<std::string, std::string>& options, std::chrono::hours otherwise) {
  const auto given = options.find("days");
  return given == options.end() ? otherwise : std::chrono::hours(24 * std::stoll(given->second));
}

std::string date(uint64_t unix_ms) {
  const auto seconds = static_cast<std::time_t>(unix_ms / 1000);
  std::tm tm{};
  gmtime_r(&seconds, &tm);
  char out[32];
  std::strftime(out, sizeof(out), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: dannan-ca init|issue|revoke|show ...\n";
    return 2;
  }
  const std::string command = argv[1];
  try {
    if (command == "show") {
      if (argc != 3) throw std::runtime_error("usage: dannan-ca show FILE");
      std::ifstream input(argv[2], std::ios::binary);
      if (!input) throw std::runtime_error(std::string("cannot read ") + argv[2]);
      const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      if (bytes.rfind("-----BEGIN CERTIFICATE-----", 0) == 0) {
        const auto cert = identity::Certificate::from_pem(bytes);
        std::printf("certificate\n  subject %s\n  %s\n  serial %llu\n  valid %s to %s\n  fingerprint %s\n", cert.subject().c_str(),
                    cert.is_ca() ? "a CA" : ("node " + (cert.node_id() ? identity::to_string(*cert.node_id()) : std::string("none"))).c_str(),
                    static_cast<unsigned long long>(cert.serial()), date(cert.not_before_unix_ms()).c_str(), date(cert.not_after_unix_ms()).c_str(),
                    identity::hex(cert.fingerprint()).c_str());
        if (cert.is_ca()) std::printf("  cluster %s\n", identity::hex(identity::cluster_id_of(cert)).c_str());
        return 0;
      }
      const auto revocations = identity::decode_revocations(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
      for (const auto& revocation : revocations)
        std::printf("revocation\n  node %s\n  serial %llu%s\n  at %s\n  reason %s\n", identity::to_string(revocation.node).c_str(),
                    static_cast<unsigned long long>(revocation.serial), revocation.serial ? "" : " (every certificate)",
                    date(revocation.revoked_unix_ms).c_str(), revocation.reason.c_str());
      return 0;
    }
    const auto options = parse(argc, argv, 2);
    if (command == "init") {
      const std::filesystem::path dir = need(options, "dir");
      const auto name = need(options, "name");
      if (std::filesystem::exists(dir / "ca.pem")) throw std::runtime_error("a CA already exists in " + dir.string() + ": it is never replaced");
      // The root of a new credential stays in memory, never on disk in clear.
      auto [custody, root] = identity::KeyCustody::create_and_open(dir, key_source(options));
      const auto ca = identity::CertificateAuthority::create(name, now_ms(), custody.node_key(), days(options, identity::default_ca_validity));
      identity::durable_create_file(dir / "ca.pem", ca.certificate().pem());
      std::fprintf(stderr, "dannan-ca: CA \"%s\" made in %s, cluster %s\n", name.c_str(), dir.string().c_str(),
                   identity::hex(identity::cluster_id_of(ca.certificate())).c_str());
      // The root goes to stdout, for `systemd-creds encrypt`, and nowhere
      // else.
      if (root) std::printf("%s\n", identity::hex(root->reveal()).c_str());
      return 0;
    }
    if (command == "issue") {
      const auto ca = open_ca(options);
      const auto key = identity::unhex(need(options, "public-key"));
      if (!key) throw std::runtime_error("--public-key is hex");
      const auto cert = ca.issue(node_of(options), *key, serial_of(options), now_ms(), days(options, identity::default_node_validity));
      std::printf("%s", cert.pem().c_str());
      return 0;
    }
    if (command == "revoke") {
      const auto ca = open_ca(options);
      const auto serial = options.contains("serial") ? std::stoull(options.at("serial")) : 0;
      const auto revocation = ca.revoke(node_of(options), serial, now_ms(), need(options, "reason"));
      const auto encoded = identity::encode_revocations({revocation});
      identity::durable_create_file(need(options, "out"), std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
      return 0;
    }
    throw std::runtime_error("unknown command " + command);
  } catch (const std::exception& error) {
    std::cerr << "dannan-ca: " << error.what() << "\n";
    return 1;
  }
}
