//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The roster: the inventory's "Membership" table. Direct liveness within
// dead_after counts and gossip does not; forgetting at the horizon; the
// persisted roster (DNNMEM01, byte for byte); identity association resets;
// capabilities and adverts; all_known_reachable. Time is stepped.

#include "dannan/membership/membership.h"

#include <gtest/gtest.h>

#include <fstream>
#include <tuple>

#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

// Macha's fsync hold: this binary's fsync, which durable files call, can be held until
// told, so a case can look at what a write in progress blocks. Linux only, as in Macha:
// a symbol here preempts the C library's only where the dynamic linker looks here first.
namespace {
struct FsyncHold {
  std::mutex mutex;
  std::condition_variable changed;
  bool armed = false;
  bool holding = false;
} fsync_hold;
}  // namespace

extern "C" int fsync(int fd) {
  {
    std::unique_lock lock(fsync_hold.mutex);
    if (fsync_hold.armed) {
      fsync_hold.holding = true;
      fsync_hold.changed.notify_all();
      fsync_hold.changed.wait(lock, [] { return !fsync_hold.armed; });
      fsync_hold.holding = false;
    }
  }
  return static_cast<int>(::syscall(SYS_fsync, fd));
}
#endif

namespace dannan::membership {
namespace {

using namespace std::chrono_literals;

NodeRecord node(uint8_t n, const std::string& host = "10.0.0.1", uint16_t port = 7437) {
  NodeRecord out;
  out.id.bytes[0] = n;
  out.id.bytes[15] = 1;
  out.host = host;
  out.port = port;
  return out;
}

struct Clocks {
  test::SteppedTime steady;
  test::SteppedWall wall;
};

Membership make(Clocks& clocks, const std::filesystem::path& path = {}, std::chrono::milliseconds forget = std::chrono::milliseconds::max()) {
  return Membership(node(1, "10.0.0.1"), 30s, path, forget, &clocks.steady, &clocks.wall);
}

bool listed(const std::vector<NodeRecord>& nodes, const identity::NodeId& id) {
  return std::any_of(nodes.begin(), nodes.end(), [&](const NodeRecord& n) { return n.id == id; });
}

TEST(Membership, DirectLivenessCountsAndGossipDoesNot) {
  Clocks clocks;
  auto members = make(clocks);
  auto gossiped = node(2, "10.0.0.2");
  gossiped.seen_unix_ms = clocks.wall.unix_ms();
  members.observe(gossiped);
  members.observe(node(3, "10.0.0.3"), true);
  EXPECT_TRUE(listed(members.active(), gossiped.id));
  EXPECT_FALSE(members.directly_reachable(gossiped.id));
  EXPECT_TRUE(members.directly_reachable(node(3).id));
  EXPECT_TRUE(members.directly_reachable(node(1).id));  // self
  clocks.steady.advance(31s);
  EXPECT_FALSE(listed(members.active(), gossiped.id));
  EXPECT_FALSE(members.directly_reachable(node(3).id));
  EXPECT_TRUE(listed(members.all(), gossiped.id));
  EXPECT_TRUE(listed(members.active(), node(1).id));
}

// Absence on the steady clock: none while a node is heard of within
// dead_after (or for self, or a node never known); once it is not, the time
// it was last heard of, which a later hearing ends (Dagda asked, 2026-11-04).
TEST(Membership, AbsenceIsTheTimeANodeWasLastHeardOf) {
  Clocks clocks;
  auto members = make(clocks);
  const auto peer = node(2, "10.0.0.2");
  members.observe(peer, true);
  const auto heard = clocks.steady.now();
  EXPECT_FALSE(members.absent_since(peer.id));
  EXPECT_FALSE(members.absent_since(node(1).id));
  EXPECT_FALSE(members.absent_since(node(9).id));
  clocks.steady.advance(31s);
  EXPECT_EQ(members.absent_since(peer.id), heard);
  members.observe(peer, true);
  EXPECT_FALSE(members.absent_since(peer.id));
}

// A restart keeps how long a node has really been gone: its absence is
// rebuilt from the wall time it was last heard of, and it is not active.
TEST(Membership, AbsenceSurvivesARestart) {
  test::TempDir dir;
  const auto path = dir.path() / "members";
  Clocks clocks;
  {
    auto members = make(clocks, path);
    members.observe(node(2, "10.0.0.2"), true);
    members.observe(node(3, "10.0.0.3"), true);
  }
  clocks.wall.advance(10min);
  Clocks later;
  later.wall.advance(std::chrono::milliseconds(clocks.wall.unix_ms() - later.wall.unix_ms()));
  auto members = make(later, path);
  EXPECT_FALSE(listed(members.active(), node(2).id));
  ASSERT_TRUE(members.absent_since(node(2).id));
  EXPECT_EQ(later.steady.now() - *members.absent_since(node(2).id), std::chrono::duration_cast<contract::Clock::duration>(10min));
  // Heard of only seconds before the restart: still absent, never active
  // before it is heard from again.
  Clocks soon;
  auto again = Membership(node(1, "10.0.0.1"), 30s, path, std::chrono::milliseconds::max(), &soon.steady, &soon.wall);
  EXPECT_FALSE(listed(again.active(), node(3).id));
  EXPECT_TRUE(again.absent_since(node(3).id));
}

// A record without an address is no use to anyone; self is never a member.
TEST(Membership, IgnoresIncompleteRecordsAndItself) {
  Clocks clocks;
  auto members = make(clocks);
  auto nameless = node(2);
  nameless.host.clear();
  members.observe(nameless, true);
  members.observe(node(1, "10.9.9.9"), true);
  EXPECT_EQ(members.all().size(), 1u);
}

// The newer record wins; a direct observation of an older one only refreshes
// liveness, so gossip that superseded a handshake's record is not undone.
TEST(Membership, TheNewerRecordWinsAndDirectOnlyRefreshes) {
  Clocks clocks;
  auto members = make(clocks);
  auto first = node(2, "10.0.0.2");
  first.seen_unix_ms = 100;
  first.capabilities = 0;
  members.observe(first);
  auto older = first;
  older.seen_unix_ms = 50;
  older.capabilities = capability_inbound;
  members.observe(older, true);
  EXPECT_FALSE(members.inbound_capable(first.id));
  EXPECT_TRUE(members.directly_reachable(first.id));
  auto newer = first;
  newer.seen_unix_ms = 200;
  newer.capabilities = capability_inbound;
  members.observe(newer);
  EXPECT_TRUE(members.inbound_capable(first.id));
}

TEST(Membership, UnknownNodesReportCapableAndSelfReportsItsOwnCapabilities) {
  Clocks clocks;
  auto members = make(clocks);
  EXPECT_TRUE(members.inbound_capable(node(9).id));
  EXPECT_TRUE(members.set_inbound_capable(false));
  EXPECT_FALSE(members.set_inbound_capable(false));
  EXPECT_FALSE(members.inbound_capable(node(1).id));
  EXPECT_TRUE(members.set_capabilities(capability_relay));
  EXPECT_TRUE(members.self().relays());
  EXPECT_FALSE(members.self().inbound_capable());
  EXPECT_TRUE(members.set_path_class(PathClass::punchable));
  EXPECT_EQ(members.self().path_class, PathClass::punchable);
}

// The hosted system's adverts ride in the record; every change makes the
// record newer, so gossip carries it. Bounded: 64 adverts, keys of 1 to 256
// bytes, values of at most 1 KiB.
TEST(Membership, AdvertsRideInTheRecordAndAreBounded) {
  Clocks clocks;
  auto members = make(clocks);
  const auto before = members.self().seen_unix_ms;
  clocks.wall.advance(5ms);
  EXPECT_TRUE(members.set_advert("capacity", u64_advert(1000)));
  EXPECT_FALSE(members.set_advert("capacity", u64_advert(1000)));
  EXPECT_EQ(members.self().advert_u64("capacity"), 1000u);
  EXPECT_GT(members.self().seen_unix_ms, before);
  EXPECT_TRUE(members.clear_advert("capacity"));
  EXPECT_FALSE(members.clear_advert("capacity"));
  EXPECT_FALSE(members.self().advert("capacity"));
  EXPECT_THROW(members.set_advert("", {}), std::invalid_argument);
  EXPECT_THROW(members.set_advert(std::string(257, 'k'), {}), std::invalid_argument);
  EXPECT_THROW(members.set_advert("big", net::Bytes(1025, 0)), std::invalid_argument);
  for (int i = 0; i < 64; ++i) members.set_advert("k" + std::to_string(i), {});
  EXPECT_THROW(members.set_advert("one-more", {}), std::invalid_argument);
  EXPECT_NO_THROW(members.set_advert("k0", net::Bytes{1}));
}

// Unheard of for the horizon (never under twice dead_after): forgotten.
// Gossip that old does not teach it back; its own connection does.
TEST(Membership, ForgetsAtTheHorizonAndOldGossipDoesNotTeachBack) {
  Clocks clocks;
  auto members = make(clocks, {}, 10s);  // raised to 2 * dead_after = 60 s
  auto peer = node(2, "10.0.0.2");
  peer.seen_unix_ms = clocks.wall.unix_ms();
  members.observe(peer, true);
  clocks.steady.advance(59s);
  clocks.wall.advance(59s);
  EXPECT_EQ(members.forget_unseen(), 0u);
  clocks.steady.advance(2s);
  clocks.wall.advance(2s);
  EXPECT_EQ(members.forget_unseen(), 1u);
  EXPECT_FALSE(listed(members.all(), peer.id));
  members.observe(peer);  // gossip carrying the old record
  EXPECT_FALSE(listed(members.all(), peer.id));
  members.observe(peer, true);
  EXPECT_TRUE(listed(members.all(), peer.id));
}

// Persisted under the state path, the whole record kept; a reloaded node
// is known but not active until it is heard from.
TEST(Membership, ThePersistedRosterReloads) {
  Clocks clocks;
  test::TempDir dir;
  const auto path = dir.path() / "known-nodes";
  auto peer = node(2, "10.0.0.2");
  peer.failure_domain = "site-b";
  peer.capabilities = 0;
  peer.path_class = PathClass::relay_only;
  peer.candidates = {{"198.51.100.4", 40001, true, 77}};
  peer.adverts["capacity"] = u64_advert(5);
  peer.seen_unix_ms = 1234;
  {
    auto members = make(clocks, path);
    members.observe(peer, true);
  }
  ASSERT_TRUE(std::filesystem::exists(path));
  auto reloaded = make(clocks, path);
  const auto all = reloaded.all();
  ASSERT_TRUE(listed(all, peer.id));
  const auto found = *std::find_if(all.begin(), all.end(), [&](const NodeRecord& n) { return n.id == peer.id; });
  EXPECT_EQ(found.host, "10.0.0.2");
  EXPECT_EQ(found.failure_domain, "site-b");
  EXPECT_EQ(found, peer);
  EXPECT_EQ(found.seen_unix_ms, 1234u);
  EXPECT_FALSE(listed(reloaded.active(), peer.id));
  EXPECT_FALSE(reloaded.inbound_capable(peer.id));
}

// The file's bytes: magic, count, then per node its record as the wire
// carries it and when this node last heard of it; then the resets.
TEST(Membership, TheRosterFileLayout) {
  Clocks clocks;
  test::TempDir dir;
  const auto path = dir.path() / "known-nodes";
  auto peer = node(2, "h", 7);
  peer.failure_domain = "d";
  peer.seen_unix_ms = 5;
  peer.capabilities = 3;
  {
    auto members = make(clocks, path);
    members.observe(peer, true);
  }
  std::ifstream input(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  net::Writer expected;
  for (const char c : std::string("DNNMEM01")) expected.u8(static_cast<uint8_t>(c));
  expected.u32(1);
  encode(expected, peer);
  expected.u64(clocks.wall.unix_ms());
  expected.u32(0);
  EXPECT_EQ(bytes, expected.data());
}

TEST(Membership, ACorruptRosterRefusesToLoad) {
  Clocks clocks;
  test::TempDir dir;
  const auto path = dir.path() / "known-nodes";
  std::ofstream(path, std::ios::binary) << "DNNMEMXX garbage";
  EXPECT_THROW(make(clocks, path), net::DecodeError);
}

// A reset re-keys an endpoint: the stale member goes, the tombstone is kept
// across a restart, and only a direct observation or newer gossip brings a
// node at that endpoint back. An older epoch changes nothing.
TEST(Membership, IdentityResetsDropTheStaleMemberAndPersist) {
  Clocks clocks;
  test::TempDir dir;
  const auto path = dir.path() / "known-nodes";
  auto stale = node(2, "10.0.0.2");
  stale.seen_unix_ms = clocks.wall.unix_ms() - 10;
  IdentityReset reset;
  reset.host = "10.0.0.2";
  reset.port = 7437;
  reset.stale_node_id = stale.id;
  reset.epoch = 2;
  reset.reset_unix_ms = clocks.wall.unix_ms();
  reset.reset_by = node(1).id;
  reset.reason = "disk replaced";
  {
    auto members = make(clocks, path);
    members.observe(stale, true);
    EXPECT_TRUE(members.apply_identity_reset(reset));
    EXPECT_FALSE(listed(members.all(), stale.id));
    auto older = reset;
    older.epoch = 1;
    EXPECT_FALSE(members.apply_identity_reset(older));
    members.observe(stale);  // gossip from before the reset
    EXPECT_FALSE(listed(members.all(), stale.id));
  }
  auto reloaded = make(clocks, path);
  ASSERT_EQ(reloaded.identity_resets().size(), 1u);
  EXPECT_EQ(reloaded.identity_resets().front(), reset);
  reloaded.observe(stale);
  EXPECT_FALSE(listed(reloaded.all(), stale.id));
  reloaded.observe(stale, true);
  EXPECT_TRUE(listed(reloaded.all(), stale.id));
}

// A reset with no port covers every port on the address; one with no node
// id covers every node there.
TEST(IdentityReset, MatchesItsEndpointAndNode) {
  IdentityReset reset;
  reset.host = "10.0.0.2";
  EXPECT_TRUE(reset.matches_endpoint("10.0.0.2", 1));
  EXPECT_FALSE(reset.matches_endpoint("10.0.0.3", 1));
  EXPECT_TRUE(reset.matches_node(node(5).id));
  reset.port = 7;
  reset.stale_node_id = node(4).id;
  EXPECT_FALSE(reset.matches_endpoint("10.0.0.2", 1));
  EXPECT_TRUE(reset.matches_endpoint("10.0.0.2", 7));
  EXPECT_FALSE(reset.matches_node(node(5).id));
  EXPECT_EQ(reset.key(), "[10.0.0.2]:7");
  reset.port = 0;
  EXPECT_EQ(reset.key(), "[10.0.0.2]:*");
}

// Gossiped as a DNNRST01 set.
TEST(IdentityReset, EncodesAndRoundTrips) {
  IdentityReset reset;
  reset.host = "h";
  reset.port = 7;
  reset.stale_node_id = node(4).id;
  reset.epoch = 9;
  reset.reset_unix_ms = 11;
  reset.reset_by = node(1).id;
  reset.reason = "r";
  const auto bytes = encode_identity_resets({reset});
  net::Writer expected;
  for (const char c : std::string("DNNRST01")) expected.u8(static_cast<uint8_t>(c));
  expected.u32(1);
  expected.string("h");
  expected.u16(7);
  expected.raw(reset.stale_node_id.bytes);
  expected.u64(9);
  expected.u64(11);
  expected.raw(reset.reset_by.bytes);
  expected.string("r");
  EXPECT_EQ(bytes, expected.data());
  EXPECT_EQ(decode_identity_resets(bytes), std::vector<IdentityReset>{reset});
  auto bad = bytes;
  bad[0] = 'X';
  EXPECT_THROW(decode_identity_resets(bad), net::DecodeError);
}

// Every known node authenticated directly within dead_after, except pairs
// that both refuse inbound connections and so can never meet.
TEST(Membership, AllKnownReachableExcludesPairsThatCannotMeet) {
  Clocks clocks;
  auto members = make(clocks);
  members.observe(node(2, "10.0.0.2"), true);
  EXPECT_TRUE(members.all_known_reachable());
  auto silent = node(3, "10.0.0.3");
  silent.capabilities = 0;
  silent.seen_unix_ms = clocks.wall.unix_ms();
  members.observe(silent);
  EXPECT_FALSE(members.all_known_reachable());
  members.set_inbound_capable(false);
  EXPECT_TRUE(members.all_known_reachable());
}

// A tombstone is a freshness boundary, not a ban: gossip about the old association
// cannot bring it back, the node itself can, and a newer reset clears it again.
TEST(Membership, ANewerResetClearsAFreshAssociation) {
  Clocks clocks;
  auto members = make(clocks);
  auto stale = node(2, "10.44.1.50");
  stale.seen_unix_ms = clocks.wall.unix_ms();
  members.observe(stale, true);
  ASSERT_EQ(members.all().size(), 2u);
  IdentityReset reset;
  reset.host = stale.host;
  reset.port = stale.port;
  reset.stale_node_id = stale.id;
  reset.epoch = 1;
  reset.reset_unix_ms = clocks.wall.unix_ms();
  reset.reset_by = node(1).id;
  ASSERT_TRUE(members.apply_identity_reset(reset));
  EXPECT_EQ(members.all().size(), 1u);
  members.observe(stale, false);
  EXPECT_EQ(members.all().size(), 1u);
  stale.seen_unix_ms = reset.reset_unix_ms + 1;
  members.observe(stale, true);
  ASSERT_EQ(members.all().size(), 2u);
  auto newer = reset;
  newer.epoch = 2;
  newer.reset_unix_ms += 2;
  ASSERT_TRUE(members.apply_identity_reset(newer));
  EXPECT_EQ(members.all().size(), 1u);
  // The endpoint is not banned: a freshly authenticated id there is good.
  auto replacement = stale;
  replacement.id = node(3).id;
  replacement.seen_unix_ms = newer.reset_unix_ms + 1;
  members.observe(replacement, true);
  ASSERT_EQ(members.all().size(), 2u);
  EXPECT_EQ(members.all().back().id, replacement.id);
  // Older or equal epochs cannot roll the tombstone back.
  EXPECT_FALSE(members.apply_identity_reset(reset));
  EXPECT_FALSE(members.apply_identity_reset(newer));
}

TEST(Membership, AnAddressWideResetDropsEveryNodeThereAndOnlyThem) {
  Clocks clocks;
  auto members = make(clocks);
  const auto before = clocks.wall.unix_ms();
  auto first = node(2, "10.44.1.50", 57401);
  first.seen_unix_ms = before;
  auto second = node(3, "10.44.1.50", 57402);
  second.seen_unix_ms = before;
  auto other = node(4, "10.44.1.51", 57401);
  other.seen_unix_ms = before;
  members.observe(first, true);
  members.observe(second, true);
  members.observe(other, true);
  ASSERT_EQ(members.all().size(), 4u);
  IdentityReset reset;
  reset.host = "10.44.1.50";
  reset.epoch = 1;
  reset.reset_unix_ms = before + 1;
  reset.reset_by = node(1).id;
  ASSERT_TRUE(members.apply_identity_reset(reset));
  ASSERT_EQ(members.all().size(), 2u);
  EXPECT_TRUE(listed(members.all(), other.id));
  // Earlier gossip cannot bring either back, though the reset named no node.
  members.observe(first, false);
  members.observe(second, false);
  EXPECT_EQ(members.all().size(), 2u);
  auto replacement = first;
  replacement.id = node(5).id;
  replacement.seen_unix_ms = reset.reset_unix_ms + 1;
  members.observe(replacement, true);
  ASSERT_EQ(members.all().size(), 3u);
  members.observe(replacement, false);
  EXPECT_EQ(members.all().size(), 3u);
}

// A reloaded node fences what it remembers until each is heard directly, so an
// isolated node does not forget peers it has not rediscovered.
TEST(Membership, AReloadedNodeFencesUntilHeardDirectly) {
  test::TempDir dir;
  Clocks clocks;
  const auto path = dir.path() / "membership" / "known-nodes.bin";
  auto peer = node(2, "10.0.0.2");
  peer.seen_unix_ms = clocks.wall.unix_ms();
  {
    auto members = make(clocks, path);
    EXPECT_TRUE(members.all_known_reachable());
    members.observe(peer, false);
    EXPECT_FALSE(members.all_known_reachable());
    members.observe(peer, true);
    EXPECT_TRUE(members.all_known_reachable());
  }
  auto recovered = make(clocks, path);
  ASSERT_EQ(recovered.all().size(), 2u);
  EXPECT_FALSE(recovered.all_known_reachable());
  clocks.wall.advance(1ms);
  peer.seen_unix_ms = clocks.wall.unix_ms();
  recovered.observe(peer, true);
  EXPECT_TRUE(recovered.all_known_reachable());
  clocks.steady.advance(31s);
  EXPECT_FALSE(recovered.all_known_reachable());
}

TEST(Membership, AGossipedCapabilityChangeIsPersisted) {
  test::TempDir dir;
  Clocks clocks;
  const auto path = dir.path() / "known-nodes.bin";
  auto peer = node(2, "10.0.0.2");
  peer.capabilities = capability_inbound;
  peer.seen_unix_ms = clocks.wall.unix_ms();
  {
    auto members = make(clocks, path);
    members.observe(peer, true);
    clocks.wall.advance(1ms);
    peer.capabilities = 0;
    peer.seen_unix_ms = clocks.wall.unix_ms();
    members.observe(peer, false);
    EXPECT_FALSE(members.inbound_capable(peer.id));
  }
  auto recovered = make(clocks, path);
  EXPECT_FALSE(recovered.inbound_capable(peer.id));
}

// Forgetting counts from when a node was last heard of, by anyone: gossip holds it,
// a restart keeps the wait, and with no horizon nobody is forgotten.
TEST(Membership, ForgettingCountsFromLastHeard) {
  test::TempDir dir;
  Clocks clocks;
  const auto path = dir.path() / "known-nodes.bin";
  auto peer = node(2, "10.0.0.2");
  peer.seen_unix_ms = clocks.wall.unix_ms();
  {
    auto members = make(clocks, path, 60s);
    members.observe(peer, true);
    for (int i = 0; i < 4; ++i) {
      clocks.steady.advance(40s);
      clocks.wall.advance(40s);
      peer.seen_unix_ms = clocks.wall.unix_ms();
      members.observe(peer, false);
      EXPECT_EQ(members.forget_unseen(), 0u);
    }
    EXPECT_FALSE(members.directly_reachable(peer.id));
  }
  {
    auto recovered = make(clocks, path, 60s);
    ASSERT_EQ(recovered.all().size(), 2u);
    clocks.steady.advance(100s);
    clocks.wall.advance(100s);
    EXPECT_EQ(recovered.forget_unseen(), 1u);
    // And the file forgets it too.
    EXPECT_EQ(make(clocks, path, 60s).all().size(), 1u);
  }
  auto keeps = make(clocks);
  peer.seen_unix_ms = clocks.wall.unix_ms();
  keeps.observe(peer, true);
  clocks.steady.advance(1000h);
  clocks.wall.advance(1000h);
  EXPECT_EQ(keeps.forget_unseen(), 0u);
  EXPECT_EQ(keeps.all().size(), 2u);
}

TEST(Membership, AllAndActiveAreOneReading) {
  Clocks clocks;
  auto members = make(clocks);
  auto peer = node(2, "10.0.0.2");
  peer.seen_unix_ms = clocks.wall.unix_ms();
  members.observe(peer, true);
  auto [all, active] = members.all_and_active();
  EXPECT_EQ(all.size(), 2u);
  EXPECT_EQ(active.size(), 2u);
  clocks.steady.advance(31s);
  std::tie(all, active) = members.all_and_active();
  EXPECT_EQ(all.size(), 2u);
  EXPECT_EQ(active.size(), 1u);
}

// Encoded in one order, reaching the file in another: the older is skipped.
TEST(SequencedFile, AnOlderWriteIsSkipped) {
  test::TempDir dir;
  SequencedFile file(dir.path() / "roster");
  const auto contents = [&] {
    std::ifstream in(dir.path() / "roster");
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  file.write(2, "newer");
  file.write(1, "older");
  EXPECT_EQ(contents(), "newer");
  file.write(2, "again");
  EXPECT_EQ(contents(), "newer");
  file.write(3, "newest");
  EXPECT_EQ(contents(), "newest");
}

#if defined(__linux__)
// A roster held at its fsync holds no reader of the roster.
TEST(Membership, ARosterWriteHoldsNoReader) {
  test::TempDir dir;
  auto members = Membership(node(1, "10.0.0.1"), 30s, dir.path() / "membership" / "known-nodes.bin");
  auto peer = node(2, "10.0.0.2");
  peer.seen_unix_ms = contract::system_wall_clock().unix_ms();
  {
    std::lock_guard lock(fsync_hold.mutex);
    fsync_hold.armed = true;
  }
  std::thread writer([&] { members.observe(peer, true); });
  bool held = false;
  {
    std::unique_lock lock(fsync_hold.mutex);
    held = fsync_hold.changed.wait_for(lock, 10s, [] { return fsync_hold.holding; });
  }
  EXPECT_TRUE(held);
  auto reader = std::async(std::launch::async, [&] { return members.all().size(); });
  const bool answered = reader.wait_for(5s) == std::future_status::ready;
  {
    std::lock_guard lock(fsync_hold.mutex);
    fsync_hold.armed = false;
  }
  fsync_hold.changed.notify_all();
  writer.join();
  EXPECT_TRUE(answered);
  EXPECT_EQ(reader.get(), 2u);
}
#endif

}  // namespace
}  // namespace dannan::membership
