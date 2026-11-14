//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/identity/sealed_counters.h"

#include <array>
#include <fstream>
#include <stdexcept>

#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"
#include "dannan/net/codec.h"

namespace dannan::identity {
namespace {

// "DNNCNT01", then the nonce, the tag and the sealed body; the magic is the
// associated data. The body: a count, then each label (u16 length, bytes) and its
// value (u64), in label order.
constexpr std::array<uint8_t, 8> magic{'D', 'N', 'N', 'C', 'N', 'T', '0', '1'};
constexpr size_t max_file = 4 * 1024 * 1024;
constexpr size_t max_label = 1024;

std::span<const uint8_t, 32> key_span(const Secret& key) {
  if (key.size() != 32) throw std::invalid_argument("sealed counters need a 32-byte key");
  return key.reveal().first<32>();
}

}  // namespace

SealedCounters::SealedCounters(std::filesystem::path state, Secret key) : _path(std::move(state) / "identity" / "counters"), _key(std::move(key)) {
  (void)key_span(_key);
  if (!std::filesystem::exists(_path)) return;
  const auto refused = [&](const std::string& why) { return std::runtime_error("node-local counters at " + _path.string() + " " + why); };
  const auto size = std::filesystem::file_size(_path);
  if (size > max_file) throw refused("are not a counters file");
  net::Bytes bytes(static_cast<size_t>(size));
  std::ifstream input(_path, std::ios::binary);
  if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) throw refused("cannot be read");
  try {
    net::Reader reader(bytes);
    if (reader.fixed<8>() != magic) throw refused("are not a counters file");
    const auto nonce = reader.fixed<12>();
    const auto tag = reader.fixed<16>();
    const auto ciphertext = reader.view(reader.remaining());
    net::Bytes body;
    try {
      body = aes_gcm_open(key_span(_key), nonce, tag, ciphertext, magic);
    } catch (const std::runtime_error&) {
      throw refused("do not open: the key is not the one they were made under, or the file was altered");
    }
    net::Reader fields(body);
    const auto count = fields.u32();
    contract::Lock lock(_mutex);
    for (uint32_t i = 0; i < count; ++i) {
      const auto length = fields.u16();
      if (length > max_label) throw refused("are malformed");
      const auto label = fields.view(length);
      _values[std::string(label.begin(), label.end())] = fields.u64();
    }
    fields.finish();
  } catch (const net::DecodeError&) {
    throw refused("are malformed");
  }
}

uint64_t SealedCounters::read(std::string_view label) const {
  contract::Lock lock(_mutex);
  const auto found = _values.find(label);
  return found == _values.end() ? 0 : found->second;
}

void SealedCounters::check(std::string_view label, uint64_t value) const {
  if (label.empty()) throw std::invalid_argument("a counter needs a label");
  if (label.size() > max_label) throw std::invalid_argument("a counter's label is at most 1024 bytes");
  const auto found = _values.find(label);
  // Named by its label only: the value is the hosted system's to report.
  if (found != _values.end() && value < found->second) throw std::invalid_argument("counter " + std::string(label) + " would go down");
}

void SealedCounters::advance(std::string_view label, uint64_t value) {
  contract::Lock lock(_mutex);
  check(label, value);
  auto& slot = _values[std::string(label)];
  if (slot == value && !_dirty) return;
  slot = value;
  write();
}

void SealedCounters::advance(const std::map<std::string, uint64_t>& values) {
  contract::Lock lock(_mutex);
  for (const auto& [label, value] : values) check(label, value);
  for (const auto& [label, value] : values) _values[label] = value;
  write();
}

void SealedCounters::note(std::string_view label, uint64_t value) {
  contract::Lock lock(_mutex);
  check(label, value);
  auto& slot = _values[std::string(label)];
  if (slot == value) return;
  slot = value;
  _dirty = true;
}

bool SealedCounters::flush() {
  contract::Lock lock(_mutex);
  if (!_dirty) return false;
  write();
  return true;
}

void SealedCounters::write() {
  net::Writer body;
  body.u32(static_cast<uint32_t>(_values.size()));
  for (const auto& [label, value] : _values) {
    body.u16(static_cast<uint16_t>(label.size()));
    body.raw({reinterpret_cast<const uint8_t*>(label.data()), label.size()});
    body.u64(value);
  }
  const auto sealed = aes_gcm_seal(key_span(_key), body.data(), magic);
  net::Writer file;
  file.raw(magic);
  file.fixed(sealed.nonce);
  file.fixed(sealed.tag);
  file.raw(sealed.ciphertext);
  durable_replace_file(_path, std::string_view(reinterpret_cast<const char*>(file.data().data()), file.data().size()));
  _dirty = false;
}

}  // namespace dannan::identity
