//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/key_custody.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dannan::identity {
namespace {

constexpr std::array<uint8_t, 8> magic{'D', 'N', 'N', 'K', 'E', 'Y', '0', '1'};
constexpr uint8_t body_version = 1;
constexpr size_t max_file = 64 * 1024;

std::filesystem::path custody_path(const std::filesystem::path& state) { return state / "identity" / "custody"; }

std::span<const uint8_t> text(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

// What the header says: how to find the root.
struct Header {
  KeySource::Kind kind = KeySource::Kind::credential;
  std::string credential;
  Argon2Costs costs;
  std::array<uint8_t, 16> salt{};
  std::array<uint8_t, 16> file_id{};
};

net::Bytes encode_header(const Header& header) {
  net::Writer writer;
  writer.raw(magic);
  writer.u8(static_cast<uint8_t>(header.kind));
  if (header.kind == KeySource::Kind::credential) {
    writer.string(header.credential);
  } else {
    writer.u32(header.costs.memory_kib);
    writer.u32(header.costs.iterations);
    writer.u32(header.costs.lanes);
    writer.fixed(header.salt);
  }
  writer.fixed(header.file_id);
  return writer.take();
}

Header decode_header(net::Reader& reader) {
  if (reader.fixed<8>() != magic) throw net::DecodeError("not a key custody file");
  Header out;
  const auto kind = reader.u8();
  if (kind == static_cast<uint8_t>(KeySource::Kind::credential)) {
    out.kind = KeySource::Kind::credential;
    out.credential = reader.string(255);
  } else if (kind == static_cast<uint8_t>(KeySource::Kind::passphrase)) {
    out.kind = KeySource::Kind::passphrase;
    out.costs.memory_kib = reader.u32();
    out.costs.iterations = reader.u32();
    out.costs.lanes = reader.u32();
    // Checked before a passphrase is asked for or a byte is spent.
    if (!out.costs.valid()) throw net::DecodeError("Argon2id costs beyond the ceiling");
    out.salt = reader.fixed<16>();
  } else {
    throw net::DecodeError("unknown key source");
  }
  out.file_id = reader.fixed<16>();
  return out;
}

template <size_t N>
std::array<uint8_t, N> random_array() {
  const auto bytes = random_bytes(N);
  std::array<uint8_t, N> out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

// The root a credential holds: 32 raw bytes, or 64 hex digits with an
// optional line end, as an operator may have typed it.
Secret read_credential(const KeySource& source) {
  auto directory = source.credentials_directory;
  if (directory.empty()) {
    const char* from_systemd = std::getenv("CREDENTIALS_DIRECTORY");
    if (!from_systemd || !*from_systemd)
      throw std::runtime_error("the systemd credential " + source.credential + " is not loaded: CREDENTIALS_DIRECTORY is unset (LoadCredentialEncrypted=)");
    directory = from_systemd;
  }
  const auto path = directory / source.credential;
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("the systemd credential " + source.credential + " is not loaded: no " + path.string());
  SecureBytes buffer(129);
  input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
  auto got = static_cast<size_t>(input.gcount());
  if (got == 32) return Secret(buffer.span().first(32));
  while (got && (buffer.data()[got - 1] == '\n' || buffer.data()[got - 1] == '\r')) --got;
  if (got == 64) {
    SecureBytes root(32);
    const auto digit = [](uint8_t c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
      const int high = digit(buffer.data()[2 * i]);
      const int low = digit(buffer.data()[2 * i + 1]);
      if (high < 0 || low < 0) throw std::runtime_error("the systemd credential " + source.credential + " is not a key");
      root.data()[i] = static_cast<uint8_t>(high * 16 + low);
    }
    return Secret(std::move(root));
  }
  throw std::runtime_error("the systemd credential " + source.credential + " is not a key: 32 bytes or 64 hex digits expected");
}

Secret root_for(const Header& header, const KeySource& source) {
  if (header.kind == KeySource::Kind::credential) return read_credential(source);
  if (!source.ask) throw std::runtime_error("key custody is under a passphrase and none can be asked for");
  const auto passphrase = source.ask();
  if (!passphrase.size()) throw std::runtime_error("an empty passphrase");
  return argon2id(passphrase.reveal(), header.salt, header.costs);
}

Secret kek(const Secret& root, const Header& header) { return hkdf_sha256_secret(root.reveal(), header.file_id, text("dannan/custody/1")); }

std::span<const uint8_t, 32> key_of(const Secret& key) { return std::span<const uint8_t, 32>(key.reveal().data(), 32); }

}  // namespace

KeySource KeySource::systemd_credential(std::string name, std::filesystem::path directory) {
  if (name.empty() || name.find('/') != std::string::npos) throw std::invalid_argument("a credential name is a file name");
  KeySource out;
  out.kind = Kind::credential;
  out.credential = std::move(name);
  out.credentials_directory = std::move(directory);
  return out;
}

KeySource KeySource::typed_passphrase(std::function<Secret()> ask, Argon2Costs costs) {
  KeySource out;
  out.kind = Kind::passphrase;
  out.ask = std::move(ask);
  out.costs = costs;
  return out;
}

bool KeyCustody::exists(const std::filesystem::path& state) { return std::filesystem::exists(custody_path(state)); }

std::optional<Secret> KeyCustody::create(const std::filesystem::path& state, const KeySource& source) {
  if (exists(state)) throw std::runtime_error("key custody already exists at " + custody_path(state).string() + ": it is never replaced");
  Header header;
  header.kind = source.kind;
  header.credential = source.credential;
  header.costs = source.costs;
  header.salt = random_array<16>();
  header.file_id = random_array<16>();
  std::optional<Secret> root;
  std::optional<Secret> stretched;
  if (source.kind == KeySource::Kind::credential) {
    root = Secret::random(32);
  } else {
    if (!source.ask) throw std::invalid_argument("a passphrase source needs a way to ask");
    const auto passphrase = source.ask();
    if (!passphrase.size()) throw std::runtime_error("an empty passphrase");
    stretched = argon2id(passphrase.reveal(), header.salt, header.costs);
  }
  const auto& sealing_root = root ? *root : *stretched;
  const auto node_key = SigningKey::generate(SigningAlgorithm::es256);
  const auto seal_root = Secret::random(32);
  const auto pkcs8 = node_key.to_pkcs8();
  // The body is assembled in secure memory, sealed, then wiped.
  SecureBytes body(1 + 4 + pkcs8.size() + 32);
  net::Writer prefix;
  prefix.u8(body_version);
  prefix.u32(static_cast<uint32_t>(pkcs8.size()));
  std::copy(prefix.data().begin(), prefix.data().end(), body.data());
  std::copy(pkcs8.reveal().begin(), pkcs8.reveal().end(), body.data() + 5);
  std::copy(seal_root.reveal().begin(), seal_root.reveal().end(), body.data() + 5 + pkcs8.size());
  const auto header_bytes = encode_header(header);
  const auto key = kek(sealing_root, header);
  const auto sealed = aes_gcm_seal(key_of(key), body.span(), header_bytes);
  net::Writer file;
  file.raw(header_bytes);
  file.fixed(sealed.nonce);
  file.fixed(sealed.tag);
  file.bytes(sealed.ciphertext);
  durable_replace_file(custody_path(state), std::string_view(reinterpret_cast<const char*>(file.data().data()), file.data().size()));
  return root;
}

KeyCustody KeyCustody::open(const std::filesystem::path& state, const KeySource& source) {
  const auto path = custody_path(state);
  if (!std::filesystem::exists(path)) throw std::runtime_error("no key custody at " + path.string() + ": make it once, with the node's init");
  const auto size = std::filesystem::file_size(path);
  if (size > max_file) throw std::runtime_error("key custody at " + path.string() + " is not a custody file");
  net::Bytes bytes(static_cast<size_t>(size));
  std::ifstream input(path, std::ios::binary);
  if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
    throw std::runtime_error("cannot read key custody at " + path.string());
  const auto refused = [&](const std::string& why) { return std::runtime_error("key custody at " + path.string() + " " + why); };
  try {
    net::Reader reader(bytes);
    const auto header = decode_header(reader);
    if (header.kind != source.kind)
      throw refused(header.kind == KeySource::Kind::credential ? "is under the systemd credential " + header.credential + ", not a passphrase"
                                                               : "is under a passphrase, not a systemd credential");
    if (header.kind == KeySource::Kind::credential && header.credential != source.credential)
      throw refused("is under the systemd credential " + header.credential + ", not " + source.credential);
    const auto header_size = bytes.size() - reader.remaining();
    const auto nonce = reader.fixed<12>();
    const auto tag = reader.fixed<16>();
    const auto ciphertext = reader.bytes(max_file);
    reader.finish();
    const auto root = root_for(header, source);
    const auto key = kek(root, header);
    SecureBuffer body;
    try {
      body = aes_gcm_open_secure(key_of(key), nonce, tag, ciphertext, std::span(bytes).first(header_size));
    } catch (const std::runtime_error&) {
      throw refused("does not open: the key is not the one it was made under, or the file was altered");
    }
    net::Reader fields(body);
    if (fields.u8() != body_version) throw refused("has a body version this build does not know");
    const auto key_size = fields.u32();
    if (key_size > body.size()) throw refused("is malformed");
    const auto pkcs8 = fields.view(key_size);
    auto node_key = SigningKey::from_pkcs8(pkcs8);
    const auto seal_root = fields.view(32);
    fields.finish();
    return KeyCustody(std::move(node_key), Secret(seal_root));
  } catch (const net::DecodeError& error) {
    throw refused(std::string("is malformed: ") + error.what());
  }
}

Secret KeyCustody::derive(std::string_view label) const {
  if (label.empty()) throw std::invalid_argument("a derived key needs a label");
  return hkdf_sha256_secret(_seal_root.reveal(), {}, text(label));
}

}  // namespace dannan::identity
