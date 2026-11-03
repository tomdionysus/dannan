//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/membership/identity_reset.h"

#include <algorithm>
#include <array>

namespace dannan::membership {
namespace {

constexpr std::array<uint8_t, 8> magic{'D', 'N', 'N', 'R', 'S', 'T', '0', '1'};
constexpr uint32_t max_records = 65536;

}  // namespace

bool IdentityReset::matches_endpoint(std::string_view endpoint_host, uint16_t endpoint_port) const noexcept {
  return host == endpoint_host && (!port || port == endpoint_port);
}

bool IdentityReset::matches_node(const identity::NodeId& node) const noexcept { return stale_node_id == identity::NodeId{} || stale_node_id == node; }

std::string IdentityReset::key() const { return "[" + host + "]:" + (port ? std::to_string(port) : std::string("*")); }

net::Bytes encode_identity_resets(const std::vector<IdentityReset>& resets) {
  net::Writer writer;
  writer.raw(magic);
  writer.u32(static_cast<uint32_t>(resets.size()));
  for (const auto& reset : resets) {
    writer.string(reset.host);
    writer.u16(reset.port);
    writer.raw(reset.stale_node_id.bytes);
    writer.u64(reset.epoch);
    writer.u64(reset.reset_unix_ms);
    writer.raw(reset.reset_by.bytes);
    writer.string(reset.reason);
  }
  return writer.take();
}

std::vector<IdentityReset> decode_identity_resets(std::span<const uint8_t> payload) {
  net::Reader reader(payload);
  const auto got = reader.fixed<8>();
  if (got != magic) throw net::DecodeError("bad identity reset payload");
  const auto count = reader.u32();
  if (count > max_records) throw net::DecodeError("too many identity reset records");
  std::vector<IdentityReset> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    IdentityReset reset;
    reset.host = reader.string(4096);
    reset.port = reader.u16();
    reset.stale_node_id.bytes = reader.fixed<16>();
    reset.epoch = reader.u64();
    reset.reset_unix_ms = reader.u64();
    reset.reset_by.bytes = reader.fixed<16>();
    reset.reason = reader.string(4096);
    if (reset.host.empty() || !reset.epoch) throw net::DecodeError("bad identity reset record");
    out.push_back(std::move(reset));
  }
  reader.finish();
  return out;
}

}  // namespace dannan::membership
