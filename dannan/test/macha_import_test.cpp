//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// dannan-import-macha: Macha's state files, as Macha's own encoders wrote them where
// they can, read back by Dannan's loaders.

#include "tools/macha_import.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "dannan/identity/node_identity.h"
#include "dannan/membership/membership.h"
#include "dannan/net/codec.h"
#include "dannan/reach/inbound.h"
#include "dannan/status/telemetry.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::tools {
namespace {

using net::Bytes;
using namespace std::chrono_literals;

// A real roster from four Macha processes on loopback at cac5998 (the fixture's
// known-nodes-a.bin): node a's. c left and had its identity reset; d left unreset.
constexpr const char* tombstone_roster =
    "4d4143484d454d3400000002a4ee05c3006dc2bfe0a4dae3baac77f0000000093132372e302e302e3100000006736974652d62448b000001a1aba223da03000001a1aba223dadc1861892a2a9c"
    "57"
    "a5655f70d96c0bbb000000093132372e302e302e3100000006736974652d64449f000001a1aba2046603000001a1aba2049b00000001000000093132372e302e302e3144952fb1e7e241aee877"
    "64"
    "74039cc3ef54050000000000000001000001a1aba2250ad12ec1c25e8f63f14a116e42fc720e5300000017666978747572653a2074686973206e6f6465206c656674";
constexpr const char* node_a = "d12ec1c25e8f63f14a116e42fc720e53";
constexpr const char* node_b = "a4ee05c3006dc2bfe0a4dae3baac77f0";
constexpr const char* node_c = "2fb1e7e241aee8776474039cc3ef5405";
constexpr const char* node_d = "dc1861892a2a9c57a5655f70d96c0bbb";

void put_magic(net::Writer& writer, std::string_view magic) { writer.raw(std::span(reinterpret_cast<const uint8_t*>(magic.data()), magic.size())); }

Bytes from_hex(std::string_view hex) {
  Bytes out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  return out;
}

identity::NodeId id_of(std::string_view hex) {
  identity::NodeId id;
  const auto bytes = from_hex(hex);
  std::copy(bytes.begin(), bytes.end(), id.bytes.begin());
  return id;
}

void write(const std::filesystem::path& path, const Bytes& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

Bytes read(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(input)), {});
}

struct State {
  test::TempDir dir;
  std::filesystem::path path = dir.path() / "state";
  explicit State(std::string_view self = node_a) {
    std::filesystem::create_directories(path);
    std::ofstream(path / "node-id") << self << "\n";
  }
  std::filesystem::path roster() const { return path / "membership" / "known-nodes.bin"; }
  std::filesystem::path inbound() const { return path / "connectivity" / "inbound.bin"; }
  std::filesystem::path telemetry() const { return path / "telemetry" / "last-known.bin"; }
};

const ImportedFile& outcome(const ImportReport& report, std::string_view path) {
  for (const auto& file : report.files)
    if (file.path == path) return file;
  throw std::runtime_error("no such file in the report");
}

membership::Membership load(const State& state, test::SteppedTime& time, test::SteppedWall& wall) {
  membership::NodeRecord self;
  self.id = id_of(node_a);
  self.host = "127.0.0.1";
  self.port = 1;
  return membership::Membership(self, 30s, state.roster(), std::chrono::milliseconds::max(), &time, &wall);
}

TEST(MachaImport, ARealRosterKeepsItsNodesAndItsTombstone) {
  State state;
  const auto original = from_hex(tombstone_roster);
  write(state.roster(), original);
  const auto report = import_macha_state(state.path);
  EXPECT_EQ(outcome(report, "membership/known-nodes.bin").outcome, "imported");
  EXPECT_EQ(read(state.path / "membership" / "known-nodes.bin.macha"), original);
  test::SteppedTime time;
  test::SteppedWall wall(0x1a1aba30000);
  auto members = load(state, time, wall);
  const auto all = members.all();
  ASSERT_EQ(all.size(), 3u);
  const auto b = std::find_if(all.begin(), all.end(), [](const auto& node) { return node.id == id_of(node_b); });
  ASSERT_NE(b, all.end());
  EXPECT_EQ(b->failure_domain, "site-b");
  EXPECT_EQ(b->port, 0x448b);
  EXPECT_TRUE(std::any_of(all.begin(), all.end(), [](const auto& node) { return node.id == id_of(node_d); }));
  EXPECT_FALSE(std::any_of(all.begin(), all.end(), [](const auto& node) { return node.id == id_of(node_c); }));
  // Flags 3: dialable, and hosts extents, now an advert.
  EXPECT_TRUE(b->inbound_capable());
  ASSERT_TRUE(b->adverts.contains("dagda/extents"));
  EXPECT_EQ(b->adverts.at("dagda/extents"), Bytes{1});
  const auto resets = members.identity_resets();
  ASSERT_EQ(resets.size(), 1u);
  EXPECT_EQ(resets[0].host, "127.0.0.1");
  EXPECT_EQ(resets[0].port, 0x4495);
  EXPECT_EQ(resets[0].stale_node_id, id_of(node_c));
  EXPECT_EQ(resets[0].epoch, 1u);
  EXPECT_EQ(resets[0].reset_by, id_of(node_a));
  EXPECT_EQ(resets[0].reason, "fixture: this node left");
  // A roster reload fences what it remembers, as Macha's does.
  EXPECT_FALSE(members.all_known_reachable());
}

Bytes older_roster(int version) {
  net::Writer writer;
  put_magic(writer, "MACHMEM");
  writer.u8(static_cast<uint8_t>('0' + version));
  writer.u32(1);
  writer.raw(id_of(node_b).bytes);
  writer.string("10.0.0.2");
  writer.string("site");
  writer.u16(7437);
  if (version >= 2) writer.u64(1000);
  if (version >= 3) writer.u8(0);
  if (version >= 2) writer.u32(0);
  return writer.take();
}

// Macha's reader takes every version, so the import does.
TEST(MachaImport, OlderRostersAreReadAsMachaReadsThem) {
  for (const int version : {1, 2, 3}) {
    State state;
    write(state.roster(), older_roster(version));
    ImportOptions options;
    options.now_unix_ms = [] { return uint64_t{5000}; };
    (void)import_macha_state(state.path, options);
    test::SteppedTime time;
    test::SteppedWall wall(6000);
    auto members = load(state, time, wall);
    const auto all = members.all();
    ASSERT_EQ(all.size(), 2u) << version;
    EXPECT_EQ(all[1].host, "10.0.0.2");
    // Flags absent read as a dialable storage node; v3 here says none.
    EXPECT_EQ(all[1].inbound_capable(), version < 3) << version;
    EXPECT_EQ(all[1].seen_unix_ms, version >= 2 ? 1000u : 0u);
  }
}

TEST(MachaImport, ARosterThatCannotBeTrustedRefusesAndChangesNothing) {
  const auto check = [](const Bytes& bytes, std::string_view why) {
    State state;
    write(state.roster(), bytes);
    write(state.inbound(), from_hex("4d414348494e4231010000000000000001000000007"));
    EXPECT_THROW((void)import_macha_state(state.path), ImportRefused) << why;
    EXPECT_EQ(read(state.roster()), bytes) << why;
    EXPECT_FALSE(std::filesystem::exists(state.roster().string() + ".macha")) << why;
  };
  check({'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0}, "magic");
  auto truncated = from_hex(tombstone_roster);
  truncated.resize(truncated.size() - 3);
  check(truncated, "truncated");
  // Names this node itself.
  State state(node_b);
  write(state.roster(), from_hex(tombstone_roster));
  EXPECT_THROW((void)import_macha_state(state.path), ImportRefused);
}

Bytes macha_inbound(bool capable, uint64_t decided, const std::string& source) {
  net::Writer writer;
  put_magic(writer, "MACHINB1");
  writer.u8(capable ? 1 : 0);
  writer.u64(decided);
  writer.string(source);
  return writer.take();
}

TEST(MachaImport, TheInboundAnswerCarriesAcross) {
  State state;
  write(state.inbound(), macha_inbound(false, 1234, std::string("probe:") + node_b));
  EXPECT_EQ(outcome(import_macha_state(state.path), "connectivity/inbound.bin").outcome, "imported");
  test::SteppedWall wall(5000);
  reach::InboundResolver resolver(reach::Inbound::automatic, state.path, &wall);
  EXPECT_FALSE(resolver.capable());
  EXPECT_EQ(resolver.state().decided_unix_ms, 1234u);
  EXPECT_EQ(resolver.state().source, "persisted");
  EXPECT_TRUE(resolver.load_error().empty());
}

Bytes macha_telemetry() {
  const auto field = [](net::Writer& out, uint16_t id, const Bytes& value) {
    out.u16(id);
    out.u16(static_cast<uint16_t>(value.size()));
    out.raw(value);
  };
  const auto u64 = [](uint64_t value) {
    net::Writer w;
    w.u64(value);
    return w.take();
  };
  const auto u32 = [](uint32_t value) {
    net::Writer w;
    w.u32(value);
    return w.take();
  };
  const auto text = [](std::string_view value) { return Bytes(value.begin(), value.end()); };
  net::Writer body;
  const auto id = id_of(node_b);
  field(body, 1, Bytes(id.bytes.begin(), id.bytes.end()));
  field(body, 3, u64(9));
  field(body, 4, u64(1000));
  field(body, 5, text("0.92.4"));
  field(body, 9, u64(4'000'000));      // storage_capacity: hosted 1024
  field(body, 14, u64(77));            // uptime_ms: core 9
  field(body, 20, u32(2));             // peers_active: core 14
  field(body, 25, text("https://x"));  // api_endpoint: hosted 1030
  field(body, 42, Bytes{0, 0});        // playback_transcode_rates: hosted 1045
  field(body, 43, text("node-b"));     // node_name: core 21
  field(body, 99, u32(1));             // from a version this does not know
  net::Writer out;
  put_magic(out, "MACHTEL3");
  out.u32(1);
  out.bytes(body.take());
  return out.take();
}

TEST(MachaImport, TelemetryFieldsMoveToTheCoreAndTheHostedTable) {
  State state;
  write(state.telemetry(), macha_telemetry());
  const auto report = import_macha_state(state.path);
  const auto& file = outcome(report, "telemetry/last-known.bin");
  EXPECT_EQ(file.outcome, "imported");
  EXPECT_NE(file.detail.find("1 unknown field"), std::string::npos) << file.detail;
  const auto records = status::decode_telemetry_set(read(state.telemetry()));
  ASSERT_EQ(records.size(), 1u);
  const auto& record = records[0];
  namespace field = status::field;
  EXPECT_EQ(record.node_id(), id_of(node_b));
  EXPECT_EQ(record.sequence(), 9u);
  EXPECT_EQ(record.text(field::version), "0.92.4");
  EXPECT_EQ(record.u64(field::first_hosted), 4'000'000u);
  EXPECT_EQ(record.u64(field::uptime_ms), 77u);
  EXPECT_EQ(record.u32(field::peers_active), 2u);
  EXPECT_EQ(record.text(field::first_hosted + 6), "https://x");
  EXPECT_TRUE(record.get(field::first_hosted + 21).has_value());
  EXPECT_EQ(record.text(field::node_name), "node-b");
  EXPECT_FALSE(record.get(99).has_value());
}

// A bad file only an observer reads is left as it was: a node ignores it too.
TEST(MachaImport, ABadObservationalFileIsIgnoredNotFatal) {
  State state;
  const Bytes junk{'j', 'u', 'n', 'k'};
  write(state.telemetry(), junk);
  write(state.inbound(), junk);
  const auto report = import_macha_state(state.path);
  EXPECT_EQ(outcome(report, "telemetry/last-known.bin").outcome, "ignored");
  EXPECT_EQ(outcome(report, "connectivity/inbound.bin").outcome, "ignored");
  EXPECT_EQ(outcome(report, "membership/known-nodes.bin").outcome, "absent");
  EXPECT_EQ(read(state.telemetry()), junk);
}

TEST(MachaImport, ASecondRunChangesNothingAndADryRunNothingAtAll) {
  State state;
  const auto roster = from_hex(tombstone_roster);
  write(state.roster(), roster);
  ImportOptions dry;
  dry.dry_run = true;
  EXPECT_EQ(outcome(import_macha_state(state.path, dry), "membership/known-nodes.bin").outcome, "imported");
  EXPECT_EQ(read(state.roster()), roster);
  EXPECT_FALSE(std::filesystem::exists(state.roster().string() + ".macha"));
  (void)import_macha_state(state.path);
  const auto imported = read(state.roster());
  EXPECT_EQ(outcome(import_macha_state(state.path), "membership/known-nodes.bin").outcome, "already");
  EXPECT_EQ(read(state.roster()), imported);
  EXPECT_EQ(read(state.roster().string() + ".macha"), roster);
}

TEST(MachaImport, ARunningNodeOrMissingIdRefuses) {
  for (const auto* lock : {".macha.lock", ".dannan.lock"}) {
    State state;
    write(state.roster(), from_hex(tombstone_roster));
    identity::StateLock held(state.path, lock);
    EXPECT_THROW((void)import_macha_state(state.path), ImportRefused) << lock;
  }
  test::TempDir empty;
  EXPECT_THROW((void)import_macha_state(empty.path()), ImportRefused);
  EXPECT_THROW((void)import_macha_state(empty.path() / "nothing"), ImportRefused);
}

// A roster at Macha's size and with every kind of reset: all of it arrives, and Dannan's
// own loader holds it. Generated, since no long-lived roster with resets exists to copy.
TEST(MachaImport, ALargeRosterWithEveryKindOfResetArrives) {
  constexpr uint32_t count = 65536;
  State state;
  net::Writer writer;
  put_magic(writer, "MACHMEM4");
  writer.u32(count);
  const auto id = [](uint32_t n) {
    identity::NodeId out;
    out.bytes[0] = 0xf0;
    for (int i = 0; i < 4; ++i) out.bytes[static_cast<size_t>(i + 1)] = static_cast<uint8_t>(n >> (24 - 8 * i));
    return out;
  };
  for (uint32_t n = 0; n < count; ++n) {
    writer.raw(id(n).bytes);
    writer.string("10." + std::to_string((n >> 16) & 255) + "." + std::to_string((n >> 8) & 255) + "." + std::to_string(n & 255));
    writer.string("domain-" + std::to_string(n % 7));
    writer.u16(7437);
    writer.u64(1000 + n);
    writer.u8(static_cast<uint8_t>(n % 4));
    writer.u64(2000 + n);
  }
  writer.u32(3);
  const auto reset = [&](const std::string& host, uint16_t port, const identity::NodeId& stale, uint64_t epoch) {
    writer.string(host);
    writer.u16(port);
    writer.raw(stale.bytes);
    writer.u64(epoch);
    writer.u64(500);
    writer.raw(id_of(node_a).bytes);
    writer.string("reason " + std::to_string(epoch));
  };
  reset("192.0.2.1", 7437, id_of(node_c), 1);       // one node at one endpoint
  reset("192.0.2.2", 0, identity::NodeId{}, 2);     // every port, every id
  reset("192.0.2.3", 7437, identity::NodeId{}, 3);  // one endpoint, every id
  write(state.roster(), writer.take());
  const auto report = import_macha_state(state.path);
  EXPECT_NE(outcome(report, "membership/known-nodes.bin").detail.find("65536 node(s), 3 reset(s)"), std::string::npos)
      << outcome(report, "membership/known-nodes.bin").detail;
  test::SteppedTime time;
  test::SteppedWall wall(1'000'000);
  auto members = load(state, time, wall);
  const auto all = members.all();
  ASSERT_EQ(all.size(), count + 1u);
  size_t capable = 0, hosting = 0;
  for (size_t i = 1; i < all.size(); ++i) {
    capable += all[i].inbound_capable() ? 1 : 0;
    hosting += all[i].adverts.contains("dagda/extents") ? 1 : 0;
  }
  EXPECT_EQ(capable, count / 2);
  EXPECT_EQ(hosting, count / 2);
  const auto resets = members.identity_resets();
  ASSERT_EQ(resets.size(), 3u);
  const auto wide = std::find_if(resets.begin(), resets.end(), [](const auto& r) { return r.epoch == 2; });
  ASSERT_NE(wide, resets.end());
  EXPECT_EQ(wide->port, 0);
  EXPECT_EQ(wide->stale_node_id, identity::NodeId{});
  EXPECT_EQ(wide->key(), "[192.0.2.2]:*");
}

// What rollback does: the originals back in place, byte for byte, and what Dannan left beside
// them. Macha opening the result is the port branch's to show.
TEST(MachaImport, RollingBackRestoresEveryFileExactly) {
  State state;
  const auto roster = from_hex(tombstone_roster);
  const auto inbound = macha_inbound(true, 7, "default");
  const auto telemetry = macha_telemetry();
  write(state.roster(), roster);
  write(state.inbound(), inbound);
  write(state.telemetry(), telemetry);
  (void)import_macha_state(state.path);
  for (const auto& path : {state.roster(), state.inbound(), state.telemetry()}) {
    EXPECT_NE(read(path), read(path.string() + ".macha"));
    std::filesystem::copy_file(path.string() + ".macha", path, std::filesystem::copy_options::overwrite_existing);
  }
  EXPECT_EQ(read(state.roster()), roster);
  EXPECT_EQ(read(state.inbound()), inbound);
  EXPECT_EQ(read(state.telemetry()), telemetry);
}

}  // namespace
}  // namespace dannan::tools
