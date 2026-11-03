//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The open route table (the design's typed routes; the extraction document's
// seam 4) and the node record every handshake carries (protocol 23's,
// grandfathered until protocol 25).

#include <gtest/gtest.h>

#include <stdexcept>

#include "dannan/membership/node_record.h"
#include "dannan/net/routes.h"

namespace dannan::net {
namespace {

using sched::FrameType;

TEST(RouteTable, DannansOwnTypesAreRegisteredWithTheirNumbers) {
  const auto routes = RouteTable::with_dannan_types();
  EXPECT_EQ(routes.find(message::ping)->name, "ping");
  EXPECT_EQ(routes.find(message::members)->name, "members");
  EXPECT_EQ(routes.find(message::telemetry)->name, "telemetry");
  EXPECT_EQ(routes.find(message::dial_request)->name, "dial_request");
  EXPECT_EQ(routes.find(message::ok)->name, "ok");
  EXPECT_EQ(routes.find(message::error)->name, "error");
  EXPECT_EQ(static_cast<uint16_t>(message::ping), 1);
  EXPECT_EQ(static_cast<uint16_t>(message::session_retire), 12);
  EXPECT_EQ(static_cast<uint16_t>(message::cancel_transfer), 16);
  EXPECT_EQ(static_cast<uint16_t>(message::telemetry), 24);
  EXPECT_EQ(static_cast<uint16_t>(message::dial_back_probe), 44);
  EXPECT_EQ(static_cast<uint16_t>(message::dial_back_probe_reply), 120);
}

TEST(RouteTable, ARegistrationTakesItsNumberOnce) {
  auto routes = RouteTable::with_dannan_types();
  routes.add(MessageType{3}, {"have_object", Lanes::control, Frames::any, false});
  EXPECT_THROW(routes.add(MessageType{3}, {"again", Lanes::control, Frames::control_only, false}), std::invalid_argument);
  EXPECT_THROW(routes.add(message::ping, {"ping", Lanes::control, Frames::control_only, false}), std::invalid_argument);
}

// The retired numbers are never reused.
TEST(RouteTable, RetiredNumbersAreRefused) {
  auto routes = RouteTable::with_dannan_types();
  for (const uint16_t retired : {uint16_t{37}, uint16_t{38}, uint16_t{48}, uint16_t{124}})
    EXPECT_THROW(routes.add(MessageType{retired}, {"x", Lanes::control, Frames::control_only, false}), std::invalid_argument);
}

// An unregistered type travels as protocol 23's default: the control lane,
// the control frame, a request that expects a reply.
TEST(RouteTable, AnUnregisteredTypeHasTheDefaultRules) {
  const auto routes = RouteTable::with_dannan_types();
  EXPECT_EQ(routes.find(MessageType{999}), nullptr);
  EXPECT_TRUE(routes.allowed(MessageType{999}, Lane::control, FrameType::control));
  EXPECT_FALSE(routes.allowed(MessageType{999}, Lane::control, FrameType::loader));
  EXPECT_FALSE(routes.allowed(MessageType{999}, Lane::data, FrameType::control));
  EXPECT_FALSE(routes.is_notification(MessageType{999}));
}

TEST(RouteTable, LanesAndFramesAreEnforcedByRegistration) {
  auto routes = RouteTable::with_dannan_types();
  routes.add(MessageType{4}, {"get_object", Lanes::data, Frames::not_control, false});
  EXPECT_TRUE(routes.allowed(MessageType{4}, Lane::data, FrameType::foreground));
  EXPECT_FALSE(routes.allowed(MessageType{4}, Lane::data, FrameType::control));
  EXPECT_FALSE(routes.allowed(MessageType{4}, Lane::control, FrameType::foreground));
  // ping rides both lanes, so a DATA lane's dead NAT mapping is noticed.
  EXPECT_TRUE(routes.allowed(message::ping, Lane::data, FrameType::control));
  EXPECT_TRUE(routes.allowed(message::ping, Lane::control, FrameType::control));
  // Replies take the request's class.
  EXPECT_TRUE(routes.allowed(message::ok, Lane::data, FrameType::loader));
  EXPECT_TRUE(routes.allowed(message::error, Lane::control, FrameType::speculative));
  // Telemetry is gossiped as speculative or control.
  EXPECT_TRUE(routes.allowed(message::telemetry, Lane::control, FrameType::speculative));
  EXPECT_TRUE(routes.is_notification(message::telemetry));
  EXPECT_TRUE(routes.is_notification(message::dial_request));
  EXPECT_FALSE(routes.is_notification(message::ping));
}

// Health and membership answer from the fast pool; ordering is opted into.
TEST(RouteTable, HealthAndMembershipAreFastControl) {
  auto routes = RouteTable::with_dannan_types();
  EXPECT_TRUE(routes.is_fast_control(message::ping));
  EXPECT_TRUE(routes.is_fast_control(message::members));
  EXPECT_FALSE(routes.is_fast_control(message::telemetry));
  EXPECT_FALSE(routes.is_ordered(message::ping));
  routes.add(MessageType{30}, {"put_metadata_commit", Lanes::control, Frames::any, false, false, true});
  EXPECT_TRUE(routes.is_ordered(MessageType{30}));
  EXPECT_FALSE(routes.is_ordered(MessageType{999}));
}

// A probe lane authenticates and closes; nothing flows on it.
TEST(RouteTable, NothingFlowsOnTheProbeLane) {
  const auto routes = RouteTable::with_dannan_types();
  EXPECT_FALSE(routes.allowed(message::ping, Lane::probe, FrameType::control));
  EXPECT_FALSE(routes.allowed(message::ok, Lane::probe, FrameType::control));
}

TEST(RouteTable, NamesEveryTypeItKnowsAndNumbersTheRest) {
  const auto routes = RouteTable::with_dannan_types();
  EXPECT_EQ(routes.name(message::members_reply), "members_reply");
  EXPECT_EQ(routes.name(MessageType{999}), "999");
}

}  // namespace
}  // namespace dannan::net

namespace dannan::membership {
namespace {

NodeRecord sample() {
  NodeRecord node;
  node.id.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  node.host = "10.0.0.1";
  node.failure_domain = "home";
  node.port = 7437;
  node.capacity = 0x0102030405060708ull;
  node.used = 9;
  node.seen_unix_ms = 10;
  node.metadata_generation = 11;
  node.metadata_write_replicas_required = 2;
  node.flags = flag_inbound_capable;
  return node;
}

// Protocol 23's record, field by field: id, host, failure domain, port, then
// the store's figures it still carries, the write floor and the flags.
TEST(NodeRecord, EncodesProtocol23sLayout) {
  net::Writer writer;
  encode(writer, sample());
  const auto bytes = writer.take();
  ASSERT_EQ(bytes.size(), 16u + 4 + 8 + 4 + 4 + 2 + 8 * 4 + 4 + 1);
  EXPECT_EQ(bytes[0], 1);
  EXPECT_EQ(bytes[15], 16);
  EXPECT_EQ(bytes[16 + 3], 8);  // the host's length
  EXPECT_EQ(bytes[16 + 4], '1');
  const size_t port_at = 16 + 4 + 8 + 4 + 4;
  EXPECT_EQ(bytes[port_at], 0x1d);
  EXPECT_EQ(bytes[port_at + 1], 0x0d);
  EXPECT_EQ(bytes[port_at + 2], 0x01);
  EXPECT_EQ(bytes.back(), flag_inbound_capable);
}

TEST(NodeRecord, RoundTrips) {
  net::Writer writer;
  encode(writer, sample());
  const auto bytes = writer.take();
  net::Reader reader(bytes);
  EXPECT_EQ(decode(reader), sample());
  EXPECT_NO_THROW(reader.finish());
}

TEST(NodeRecord, RefusesAnOverlongHost) {
  auto node = sample();
  node.host = std::string(5000, 'h');
  net::Writer writer;
  encode(writer, node);
  const auto bytes = writer.take();
  net::Reader reader(bytes);
  EXPECT_THROW(decode(reader), net::DecodeError);
}

// Both flags set is an ordinary dialable storage node, the default.
TEST(NodeRecord, DefaultsToADialableStorageNode) {
  const NodeRecord node;
  EXPECT_TRUE(node.inbound_capable());
  EXPECT_TRUE(node.hosts_extents());
  EXPECT_EQ(flags_for(false, true), flag_hosts_extents);
}

}  // namespace
}  // namespace dannan::membership
