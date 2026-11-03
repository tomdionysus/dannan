//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/membership/node_record.h"

namespace dannan::membership {

const char* path_class_name(PathClass path) noexcept {
  switch (path) {
    case PathClass::public_path:
      return "public";
    case PathClass::punchable:
      return "punchable";
    case PathClass::relay_only:
      return "relay_only";
  }
  return "unknown";
}

std::optional<std::span<const uint8_t>> NodeRecord::advert(const std::string& key) const {
  const auto found = adverts.find(key);
  if (found == adverts.end()) return std::nullopt;
  return std::span<const uint8_t>(found->second);
}

std::optional<uint64_t> NodeRecord::advert_u64(const std::string& key) const {
  const auto value = advert(key);
  if (!value || value->size() != 8) return std::nullopt;
  uint64_t out = 0;
  for (const auto byte : *value) out = (out << 8) | byte;
  return out;
}

net::Bytes u64_advert(uint64_t value) {
  net::Writer writer;
  writer.u64(value);
  return writer.take();
}

void encode(net::Writer& writer, const NodeRecord& node) {
  writer.fixed(node.id.bytes);
  writer.string(node.host);
  writer.u16(node.port);
  writer.string(node.failure_domain);
  writer.u64(node.seen_unix_ms);
  writer.fixed(node.incarnation.bytes);
  writer.u32(node.capabilities);
  writer.u8(static_cast<uint8_t>(node.path_class));
  writer.u16(static_cast<uint16_t>(node.candidates.size()));
  for (const auto& candidate : node.candidates) {
    writer.string(candidate.host);
    writer.u16(candidate.port);
    writer.u8(candidate.confirmed ? 1 : 0);
    writer.u64(candidate.confirmed_unix_ms);
  }
  writer.u16(static_cast<uint16_t>(node.adverts.size()));
  for (const auto& [key, value] : node.adverts) {
    writer.string(key);
    writer.bytes(value);
  }
}

NodeRecord decode(net::Reader& reader) {
  NodeRecord node;
  node.id.bytes = reader.fixed<16>();
  node.host = reader.string(4096);
  node.port = reader.u16();
  node.failure_domain = reader.string(4096);
  node.seen_unix_ms = reader.u64();
  node.incarnation.bytes = reader.fixed<16>();
  node.capabilities = reader.u32();
  const auto path = reader.u8();
  if (path > static_cast<uint8_t>(PathClass::relay_only)) throw net::DecodeError("bad path class");
  node.path_class = static_cast<PathClass>(path);
  const auto candidates = reader.u16();
  if (candidates > max_candidates) throw net::DecodeError("too many candidates");
  for (uint16_t i = 0; i < candidates; ++i) {
    Candidate candidate;
    candidate.host = reader.string(4096);
    candidate.port = reader.u16();
    const auto confirmed = reader.u8();
    if (confirmed > 1) throw net::DecodeError("bad candidate");
    candidate.confirmed = confirmed != 0;
    candidate.confirmed_unix_ms = reader.u64();
    node.candidates.push_back(std::move(candidate));
  }
  const auto adverts = reader.u16();
  if (adverts > max_adverts) throw net::DecodeError("too many adverts");
  for (uint16_t i = 0; i < adverts; ++i) {
    auto key = reader.string(max_advert_key);
    if (key.empty()) throw net::DecodeError("empty advert key");
    auto value = reader.bytes(max_advert_value);
    if (!node.adverts.emplace(std::move(key), std::move(value)).second) throw net::DecodeError("duplicate advert");
  }
  return node;
}

}  // namespace dannan::membership
