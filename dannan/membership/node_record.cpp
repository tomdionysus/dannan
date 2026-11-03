//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/membership/node_record.h"

namespace dannan::membership {

void encode(net::Writer& writer, const NodeRecord& node) {
  writer.fixed(node.id.bytes);
  writer.string(node.host);
  writer.string(node.failure_domain);
  writer.u16(node.port);
  writer.u64(node.capacity);
  writer.u64(node.used);
  writer.u64(node.seen_unix_ms);
  writer.u64(node.metadata_generation);
  writer.u32(node.metadata_write_replicas_required);
  writer.u8(node.flags);
}

NodeRecord decode(net::Reader& reader) {
  NodeRecord node;
  node.id.bytes = reader.fixed<16>();
  node.host = reader.string(4096);
  node.failure_domain = reader.string(4096);
  node.port = reader.u16();
  node.capacity = reader.u64();
  node.used = reader.u64();
  node.seen_unix_ms = reader.u64();
  node.metadata_generation = reader.u64();
  node.metadata_write_replicas_required = reader.u32();
  node.flags = reader.u8();
  return node;
}

}  // namespace dannan::membership
