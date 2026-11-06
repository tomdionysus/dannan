//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Telemetry: the inventory's "Telemetry and status" rows. The record is
// tagged fields (DNNTEL01): Dannan fills its core fields, below 1024, and
// carries the hosted system's own, numbered from 1024, as they come.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>

#include "dannan/status/telemetry.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::status {
namespace {

using namespace std::chrono_literals;

identity::NodeId id(uint8_t n) {
  identity::NodeId out;
  out.bytes[0] = n;
  return out;
}

Telemetry sample(uint8_t node, uint64_t sequence, uint64_t observed, const identity::NodeId& boot = id(9)) {
  Telemetry t;
  t.set_bytes(field::node_id, id(node).bytes);
  t.set_bytes(field::boot_id, boot.bytes);
  t.set_u64(field::sequence, sequence);
  t.set_u64(field::observed_unix_ms, observed);
  t.set_text(field::host, "10.0.0." + std::to_string(node));
  t.set_u16(field::port, 7437);
  t.set_phase(Phase::ready);
  return t;
}

TEST(Telemetry, FieldsAreTaggedAndDefaultsOmitted) {
  Telemetry t;
  t.set_u64(field::uptime_ms, 0);
  t.set_text(field::version, "");
  EXPECT_TRUE(t.fields().empty());
  t.set_u32(field::cpu_cores, 8);
  t.set_text(field::version, "0.1.0");
  t.set_u32(field::cpu_cores, 16);  // replaced in place, not appended
  ASSERT_EQ(t.fields().size(), 2u);
  EXPECT_EQ(t.fields()[0].first, field::cpu_cores);
  EXPECT_EQ(t.u32(field::cpu_cores), 16u);
  EXPECT_EQ(t.text(field::version), "0.1.0");
  EXPECT_EQ(t.u64(field::uptime_ms), 0u);
  t.set_u32(field::cpu_cores, 0);
  EXPECT_EQ(t.fields().size(), 1u);
}

// A record: u32 length, then u16 id, u16 length, value per field; a set:
// DNNTEL01, u32 count, records.
TEST(Telemetry, EncodesAsTel3) {
  Telemetry t;
  t.set_u16(field::port, 7);
  t.set_text(field::host, "h");
  const auto bytes = encode_telemetry_set({t});
  net::Writer expected;
  for (const char c : std::string("DNNTEL01")) expected.u8(static_cast<uint8_t>(c));
  expected.u32(1);
  expected.u32(4 + 2 + 4 + 1);
  expected.u16(field::port);
  expected.u16(2);
  expected.u16(7);
  expected.u16(field::host);
  expected.u16(1);
  expected.u8('h');
  EXPECT_EQ(bytes, expected.data());
}

TEST(Telemetry, RoundTripsAndKeepsFieldsItDoesNotKnow) {
  auto t = sample(2, 5, 100);
  t.set_bytes(900, net::Bytes{1, 2, 3});
  const auto back = decode_telemetry_set(encode_telemetry_set({t}));
  ASSERT_EQ(back.size(), 1u);
  EXPECT_EQ(back[0].fields(), t.fields());
  EXPECT_EQ(back[0].node_id(), id(2));
  EXPECT_EQ(back[0].sequence(), 5u);
  EXPECT_EQ(back[0].host(), "10.0.0.2");
  EXPECT_EQ(back[0].port(), 7437u);
  EXPECT_EQ(back[0].phase(), Phase::ready);
  EXPECT_EQ(decode_telemetry(encode_telemetry(t)).fields(), t.fields());
}

TEST(Telemetry, RefusesWhatItCannotTrust) {
  auto bad_set = encode_telemetry_set({sample(2, 5, 100)});
  bad_set[7] = '2';
  EXPECT_THROW(decode_telemetry_set(bad_set), net::DecodeError);
  Telemetry no_sequence;
  no_sequence.set_bytes(field::node_id, id(2).bytes);
  EXPECT_THROW(decode_telemetry_set(encode_telemetry_set({no_sequence})), net::DecodeError);
  auto wrong_width = sample(2, 5, 100);
  wrong_width.set_bytes(field::port, net::Bytes{1, 2, 3});
  EXPECT_THROW(decode_telemetry_set(encode_telemetry_set({wrong_width})), net::DecodeError);
  auto bad_phase = sample(2, 5, 100);
  bad_phase.set_bytes(field::phase, net::Bytes{3});
  EXPECT_THROW(decode_telemetry_set(encode_telemetry_set({bad_phase})), net::DecodeError);
}

// An omitted phase reads as ready, which is why a record always states it.
TEST(Telemetry, AMissingPhaseReadsAsReady) {
  Telemetry t;
  EXPECT_EQ(t.phase(), Phase::ready);
  t.set_phase(Phase::starting);
  EXPECT_EQ(t.phase(), Phase::starting);
  ASSERT_EQ(t.fields().size(), 1u);  // stated even though starting is zero
}

// A layout puts the fields in its order; others keep theirs, after.
TEST(Telemetry, ALayoutOrdersTheFields) {
  Telemetry t;
  t.set_u64(field::uptime_ms, 1);
  t.set_bytes(901, net::Bytes{1});
  t.set_text(field::host, "h");
  t.order_by({field::host, field::uptime_ms});
  ASSERT_EQ(t.fields().size(), 3u);
  EXPECT_EQ(t.fields()[0].first, field::host);
  EXPECT_EQ(t.fields()[1].first, field::uptime_ms);
  EXPECT_EQ(t.fields()[2].first, 901);
}

struct Store {
  test::SteppedTime time;
  test::SteppedWall wall;
  TelemetryStore store{id(1), {}, &time, &wall};
};

// Within one boot a higher sequence wins; a new boot wins unless it is more
// than a minute older than what is held.
TEST(TelemetryStore, KeepsTheNewestRecordPerNode) {
  Store s;
  s.store.observe(sample(2, 5, 1000));
  s.store.observe(sample(2, 4, 2000));
  EXPECT_EQ(s.store.all().front().sequence(), 5u);
  s.store.observe(sample(2, 6, 900));
  EXPECT_EQ(s.store.all().front().sequence(), 6u);
  s.store.observe(sample(2, 1, 900 - 60001, id(8)));
  EXPECT_EQ(s.store.all().front().sequence(), 6u);
  s.store.observe(sample(2, 1, 900 - 59000, id(8)));
  EXPECT_EQ(s.store.all().front().sequence(), 1u);
  Telemetry anonymous;
  anonymous.set_u64(field::sequence, 3);
  s.store.observe(anonymous);
  EXPECT_EQ(s.store.all().size(), 1u);
}

TEST(TelemetryStore, RecentIsBoundedByAgeAndCountNewestFirst) {
  Store s;
  s.store.observe(sample(2, 1, 100));
  s.time.advance(10s);
  s.store.observe(sample(3, 1, 300));
  s.store.observe(sample(4, 1, 200));
  const auto recent = s.store.recent(5s, 10);
  ASSERT_EQ(recent.size(), 2u);
  EXPECT_EQ(recent[0].node_id(), id(3));
  EXPECT_EQ(s.store.recent(60s, 1).size(), 1u);
  const auto views = s.store.views(5s);
  EXPECT_EQ(std::count_if(views.begin(), views.end(), [](const TelemetryView& v) { return v.fresh; }), 2);
}

TEST(TelemetryStore, AResetDropsTheStaleRecordAndOldGossipStaysOut) {
  Store s;
  s.store.observe(sample(2, 1, 100));
  membership::IdentityReset reset;
  reset.host = "10.0.0.2";
  reset.epoch = 1;
  reset.reset_unix_ms = 500;
  s.store.apply_identity_reset(reset);
  EXPECT_TRUE(s.store.all().empty());
  s.store.observe(sample(2, 2, 400));
  EXPECT_TRUE(s.store.all().empty());
  s.store.observe(sample(2, 2, 400), true);
  EXPECT_EQ(s.store.all().size(), 1u);
}

// Persisted as a set, newest per node, at most 1024; a corrupt file never
// stops a start.
TEST(TelemetryStore, PersistsAndIgnoresACorruptFile) {
  test::TempDir dir;
  const auto path = dir.path() / "telemetry";
  {
    Store s;
    TelemetryStore store(id(1), path, &s.time, &s.wall);
    store.observe(sample(2, 1, 100));
    store.persist();
  }
  Store s;
  TelemetryStore reloaded(id(1), path, &s.time, &s.wall);
  ASSERT_EQ(reloaded.persisted().size(), 1u);
  EXPECT_EQ(reloaded.persisted().front().node_id(), id(2));
  std::ofstream(path, std::ios::binary) << "garbage";
  TelemetryStore corrupt(id(1), path, &s.time, &s.wall);
  EXPECT_TRUE(corrupt.persisted().empty());
}

// The local record: core fields from the node, the hosted system's own from
// its callback, in the layout's order; traffic rates over the interval.
TEST(TelemetryStore, RefreshesTheLocalRecord) {
  Store s;
  LocalFigures figures;
  figures.self.id = id(1);
  figures.self.host = "10.0.0.1";
  figures.self.port = 7437;
  figures.version = "0.1.0";
  figures.phase = Phase::recovering;
  figures.peers_known = 3;
  figures.peers_active = 2;
  TrafficTotals traffic;
  traffic.in_bytes[1] = 100;
  figures.traffic = traffic;
  const auto first = s.store.refresh_local(figures, [](Telemetry& t) { t.set_u64(1100, 77); });
  EXPECT_EQ(first.node_id(), id(1));
  EXPECT_EQ(first.sequence(), 1u);
  EXPECT_EQ(first.phase(), Phase::recovering);
  EXPECT_EQ(first.u64(1100), 77u);
  EXPECT_EQ(field::first_hosted, 1024);
  EXPECT_EQ(first.text(field::version), "0.1.0");
  EXPECT_GT(first.u32(field::cpu_cores), 0u);
  ASSERT_TRUE(s.store.local());
  EXPECT_EQ(s.store.local()->sequence(), 1u);
  // The core fields first, the hosted system's after them.
  std::vector<uint16_t> order;
  for (const auto& [field_id, _] : first.fields()) order.push_back(field_id);
  EXPECT_EQ(order.back(), 1100);
  EXPECT_TRUE(std::is_sorted(order.begin(), order.end()));
  s.time.advance(2s);
  traffic.in_bytes[1] = 300;
  figures.traffic = traffic;
  const auto second = s.store.refresh_local(figures);
  EXPECT_EQ(second.sequence(), 2u);
  const auto classes = second.traffic();
  ASSERT_FALSE(classes.empty());
  EXPECT_EQ(classes[0].frame_class, 1);
  EXPECT_EQ(classes[0].in_bytes, 300u);
  EXPECT_EQ(classes[0].in_bytes_per_s, 100u);
  EXPECT_EQ(second.traffic_window_ms(), 2000u);
}

// A hosted system's own fields: every kind a record like Macha's needs (wide and
// narrow numbers, text, a variable-length list), in the layout it names. They cross
// the wire and the persisted file as they were, and a reader that does not know them
// keeps them. The field table is the hosted system's; these ids are only examples.
TEST(Telemetry, AHostedLayoutOfEveryKindRoundTrips) {
  constexpr uint16_t wide = field::first_hosted;
  constexpr uint16_t narrow = field::first_hosted + 1;
  constexpr uint16_t text_field = field::first_hosted + 2;
  constexpr uint16_t list = field::first_hosted + 3;
  constexpr uint16_t unset = field::first_hosted + 4;
  Telemetry record;
  record.set_bytes(field::node_id, identity::NodeId::random().bytes);
  record.set_u64(field::sequence, 7);
  record.set_u64(wide, 4'398'046'511'104ull);
  record.set_u32(narrow, 32);
  record.set_text(text_field, "https://node.example/api");
  const net::Bytes rates{0, 5, 'v', 'i', 'd', 'e', 'o', 0, 0, 4, 56};
  record.set_bytes(list, rates);
  record.set_u64(unset, 0);
  record.order_by({list, wide, narrow, text_field});
  EXPECT_EQ(record.fields().front().first, list);
  const auto check = [&](const Telemetry& got) {
    EXPECT_EQ(got.u64(wide), 4'398'046'511'104ull);
    EXPECT_EQ(got.u32(narrow), 32u);
    EXPECT_EQ(got.text(text_field), "https://node.example/api");
    const auto bytes = got.get(list);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(net::Bytes(bytes->begin(), bytes->end()), rates);
    // Zero is not written, and absent reads as "did not say".
    EXPECT_FALSE(got.get(unset).has_value());
    EXPECT_EQ(got.u64(unset), 0u);
  };
  check(decode_telemetry(encode_telemetry(record)));
  const auto set = decode_telemetry_set(encode_telemetry_set({record, record}));
  ASSERT_EQ(set.size(), 2u);
  check(set[1]);
  // A figure read at the wrong width is refused, not guessed.
  const auto decoded = decode_telemetry(encode_telemetry(record));
  EXPECT_THROW((void)decoded.u32(wide), net::DecodeError);
}

TEST(Telemetry, ATruncatedRecordIsRefused) {
  Telemetry record;
  record.set_bytes(field::node_id, identity::NodeId::random().bytes);
  record.set_u64(field::sequence, 7);
  record.set_text(field::first_hosted, "some text");
  const auto bytes = encode_telemetry(record);
  for (size_t keep = 0; keep < bytes.size(); ++keep) {
    EXPECT_THROW((void)decode_telemetry(std::span<const uint8_t>(bytes).first(keep)), net::DecodeError) << keep;
  }
}

TEST(Telemetry, AnUnknownFieldLeavesTheNextRecordWhole) {
  Telemetry first;
  first.set_bytes(field::node_id, identity::NodeId::random().bytes);
  first.set_u64(field::sequence, 1);
  first.set_bytes(60000, net::Bytes(50, 9));
  Telemetry second;
  second.set_bytes(field::node_id, identity::NodeId::random().bytes);
  second.set_u64(field::sequence, 2);
  const auto set = decode_telemetry_set(encode_telemetry_set({first, second}));
  ASSERT_EQ(set.size(), 2u);
  EXPECT_EQ(set[0].get(60000)->size(), 50u);
  EXPECT_EQ(set[1].sequence(), 2u);
  EXPECT_EQ(set[1].node_id(), second.node_id());
}

// A hosted layout of every kind Macha's record uses, in a mixed set with
// records that do not have them, through the wire and the disk.
TEST(TelemetryStore, AHostedLayoutOfEveryKindSurvivesTheDisk) {
  test::TempDir dir;
  const auto path = dir.path() / "telemetry";
  constexpr uint16_t used = 1024, count = 1025, label = 1026, list = 1027;
  const net::Bytes rates{1, 2, 3, 4, 5};
  auto record = sample(2, 1, 100);
  record.set_u64(used, 0x0102030405060708ULL);
  record.set_u32(count, 77);
  record.set_text(label, "lounge");
  record.set_bytes(list, rates);
  {
    Store s;
    TelemetryStore store(id(1), path, &s.time, &s.wall);
    store.observe(record);
    store.observe(sample(3, 1, 100));
    store.persist();
  }
  Store s;
  TelemetryStore reloaded(id(1), path, &s.time, &s.wall);
  ASSERT_EQ(reloaded.persisted().size(), 2u);
  for (const auto& one : reloaded.persisted()) {
    if (one.node_id() != id(2)) {
      EXPECT_EQ(one.u64(used), 0u);
      EXPECT_EQ(one.text(label), "");
      continue;
    }
    EXPECT_EQ(one.u64(used), 0x0102030405060708ULL);
    EXPECT_EQ(one.u32(count), 77u);
    EXPECT_EQ(one.text(label), "lounge");
    const auto bytes = one.get(list);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(net::Bytes(bytes->begin(), bytes->end()), rates);
  }
}

}  // namespace
}  // namespace dannan::status
