//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Dannan against Macha's own code in one process: whatever protocol 23
// says, both sides compute it the same way. Macha is read-only here, a
// pinned export of a tagged release.

#include <gtest/gtest.h>

#include <fstream>

#include "codec.hpp"
#include "crypto.hpp"
#include "dannan/identity/cluster_key.h"
#include "dannan/membership/node_record.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "types.hpp"

namespace dannan {
namespace {

// The same key file is the same cluster to both.
TEST(Interop, TheClusterKeyDerivesAsMachasDoes) {
  test::TempDir dir;
  for (const auto* secret : {"one secret\n", "another\r\n\n", "x"}) {
    const auto path = dir.path() / "cluster.key";
    std::ofstream(path, std::ios::binary | std::ios::trunc) << secret;
    const auto ours = identity::load_cluster_key(path);
    const auto theirs = macha::load_cluster_keys(path);
    EXPECT_EQ(ours.master, theirs.master);
    EXPECT_EQ(ours.auth, theirs.auth);
    EXPECT_EQ(ours.cluster_id, theirs.cluster_id);
    EXPECT_EQ(ours.derive("macha/storage/v1"), theirs.storage);
  }
}

membership::NodeRecord ours_sample() {
  membership::NodeRecord node;
  node.id = identity::NodeId::random();
  node.host = "media-node-2.example.net";
  node.failure_domain = "site-a";
  node.port = 7437;
  node.capacity = 10ull << 40;
  node.used = 3ull << 30;
  node.seen_unix_ms = 1793700000000ull;
  node.metadata_generation = 4242;
  node.metadata_write_replicas_required = 2;
  node.flags = membership::flags_for(false, true);
  return node;
}

macha::NodeInfo theirs_of(const membership::NodeRecord& ours) {
  macha::NodeInfo node;
  node.id.bytes = ours.id.bytes;
  node.host = ours.host;
  node.failure_domain = ours.failure_domain;
  node.port = ours.port;
  node.capacity = ours.capacity;
  node.used = ours.used;
  node.seen_unix_ms = ours.seen_unix_ms;
  node.metadata_generation = ours.metadata_generation;
  node.metadata_write_replicas_required = ours.metadata_write_replicas_required;
  node.flags = ours.flags;
  return node;
}

// The record every handshake and every roster exchange carries.
TEST(Interop, TheNodeRecordEncodesAsMachasDoes) {
  const auto ours = ours_sample();
  net::Writer writer;
  membership::encode(writer, ours);
  macha::Writer theirs;
  macha::encode_node_info(theirs, theirs_of(ours));
  EXPECT_EQ(writer.data(), theirs.data());

  macha::Reader reader(theirs.data());
  const auto decoded = macha::decode_node_info(reader);
  net::Reader back(writer.data());
  const auto mine = membership::decode(back);
  EXPECT_EQ(mine.host, decoded.host);
  EXPECT_EQ(mine.id.bytes, decoded.id.bytes);
  EXPECT_EQ(mine.flags, decoded.flags);
}

}  // namespace
}  // namespace dannan
