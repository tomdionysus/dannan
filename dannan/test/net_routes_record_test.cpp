//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The open route table (the design's typed routes) with wire v1's numbers,
// and the node record v1 every handshake carries (wire v1, sections 3 and
// 4).

#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <vector>

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
  // Requests from 1, replies from 100, error last of the core block's.
  const std::vector<std::pair<MessageType, uint16_t>> numbers{{message::ping, 1},
                                                              {message::members, 2},
                                                              {message::session_retire, 3},
                                                              {message::promote_read_ahead, 4},
                                                              {message::promote_foreground, 5},
                                                              {message::cancel_transfer, 6},
                                                              {message::telemetry, 7},
                                                              {message::identity_resets, 8},
                                                              {message::dial_request, 9},
                                                              {message::dial_back, 10},
                                                              {message::punch_request, 11},
                                                              {message::punch_go, 12},
                                                              {message::relay_open, 13},
                                                              {message::relay_offer, 14},
                                                              {message::relay_data, 15},
                                                              {message::relay_close, 16},
                                                              {message::ok, 100},
                                                              {message::members_reply, 101},
                                                              {message::telemetry_reply, 102},
                                                              {message::identity_resets_reply, 103},
                                                              {message::dial_back_reply, 104},
                                                              {message::punch_reply, 105},
                                                              {message::relay_open_reply, 106},
                                                              {message::relay_offer_reply, 107},
                                                              {message::error, 127}};
  for (const auto& [type, number] : numbers) {
    EXPECT_EQ(static_cast<uint16_t>(type), number);
    EXPECT_NE(routes.find(type), nullptr) << number;
  }
}

TEST(RouteTable, ARegistrationTakesItsNumberOnce) {
  auto routes = RouteTable::with_dannan_types();
  routes.add(MessageType{1027}, {"have_object", Lanes::control, Frames::any, false});
  EXPECT_THROW(routes.add(MessageType{1027}, {"again", Lanes::control, Frames::control_only, false}), std::invalid_argument);
  EXPECT_THROW(routes.add(message::ping, {"ping", Lanes::control, Frames::control_only, false}), std::invalid_argument);
}

// 1 to 1023 are Dannan's, known to this version or not: a hosted system
// numbers its types from 1024.
TEST(RouteTable, UserTypesStartAt1024) {
  auto routes = RouteTable::with_dannan_types();
  EXPECT_EQ(first_user_type, 1024);
  for (const uint16_t reserved : {uint16_t{0}, uint16_t{17}, uint16_t{500}, uint16_t{1023}})
    EXPECT_THROW(routes.add(MessageType{reserved}, {"x", Lanes::control, Frames::control_only, false}), std::invalid_argument);
  EXPECT_NO_THROW(routes.add(MessageType{1024}, {"first", Lanes::control, Frames::control_only, false}));
  EXPECT_TRUE(routes.is_core(MessageType{17}));
  EXPECT_FALSE(routes.is_core(MessageType{1024}));
}

// Core types are Dannan's and keep Dannan's rules; every other number is a
// user type, under the user's control (Tom, 2026-11-03). One nobody on this
// node registered is not policed: it travels on either lane in any class and
// goes to the user's handler, which may decline it, so a peer hosting
// something this node does not is never cut off for it. (Was: protocol 23's
// default, control lane and control frame only; superseded.)
TEST(RouteTable, AnUnregisteredUserTypeIsNotPoliced) {
  const auto routes = RouteTable::with_dannan_types();
  EXPECT_FALSE(routes.is_core(MessageType{1999}));
  EXPECT_TRUE(routes.allowed(MessageType{1999}, Lane::control, FrameType::control));
  EXPECT_TRUE(routes.allowed(MessageType{1999}, Lane::control, FrameType::loader));
  EXPECT_TRUE(routes.allowed(MessageType{1999}, Lane::data, FrameType::speculative));
  EXPECT_FALSE(routes.allowed(MessageType{1999}, Lane::probe, FrameType::control));
}

TEST(RouteTable, CoreTypesAreDannansAndCannotBeRedefined) {
  auto routes = RouteTable::with_dannan_types();
  for (const auto type :
       {message::ping, message::members, message::session_retire, message::cancel_transfer, message::dial_request, message::ok, message::error})
    EXPECT_TRUE(routes.is_core(type));
  EXPECT_THROW(routes.add(message::ping, {"mine", Lanes::both, Frames::any, false}), std::invalid_argument);
  routes.add(MessageType{1028}, {"get_object", Lanes::data, Frames::not_control, false});
  EXPECT_FALSE(routes.is_core(MessageType{1028}));
  // Core rules still hold.
  EXPECT_FALSE(routes.allowed(message::members, Lane::data, FrameType::control));
  EXPECT_FALSE(routes.allowed(message::ping, Lane::control, FrameType::loader));
}

TEST(RouteTable, LanesAndFramesAreEnforcedByRegistration) {
  auto routes = RouteTable::with_dannan_types();
  routes.add(MessageType{1028}, {"get_object", Lanes::data, Frames::not_control, false});
  EXPECT_TRUE(routes.allowed(MessageType{1028}, Lane::data, FrameType::foreground));
  EXPECT_FALSE(routes.allowed(MessageType{1028}, Lane::data, FrameType::control));
  EXPECT_FALSE(routes.allowed(MessageType{1028}, Lane::control, FrameType::foreground));
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
  routes.add(MessageType{1054}, {"put_metadata_commit", Lanes::control, Frames::any, false, false, true});
  EXPECT_TRUE(routes.is_ordered(MessageType{1054}));
  EXPECT_FALSE(routes.is_ordered(MessageType{1999}));
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
  EXPECT_EQ(routes.name(MessageType{1999}), "1999");
}

}  // namespace
}  // namespace dannan::net

namespace dannan::membership {
namespace {

NodeRecord sample() {
  NodeRecord node;
  node.id.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  node.host = "10.0.0.1";
  node.port = 7437;
  node.failure_domain = "home";
  node.seen_unix_ms = 10;
  node.incarnation.bytes[0] = 0x77;
  node.capabilities = capability_inbound | capability_relay;
  node.path_class = PathClass::punchable;
  node.candidates = {{"198.51.100.7", 40000, true, 99}};
  node.adverts["capacity"] = u64_advert(0x0102030405060708ull);
  return node;
}

// Wire v1's record, field by field: id, host, port, failure domain, seen,
// incarnation, capabilities, path class, candidates, adverts.
TEST(NodeRecord, EncodesWireV1sLayout) {
  net::Writer writer;
  encode(writer, sample());
  net::Writer expected;
  expected.fixed(sample().id.bytes);
  expected.string("10.0.0.1");
  expected.u16(7437);
  expected.string("home");
  expected.u64(10);
  expected.fixed(sample().incarnation.bytes);
  expected.u32(3);
  expected.u8(1);
  expected.u16(1);
  expected.string("198.51.100.7");
  expected.u16(40000);
  expected.u8(1);
  expected.u64(99);
  expected.u16(1);
  expected.string("capacity");
  expected.bytes(net::Bytes{1, 2, 3, 4, 5, 6, 7, 8});
  EXPECT_EQ(writer.data(), expected.data());
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

// Bounded: 16 candidates, 64 adverts of at most 1 KiB under keys of at most
// 256 bytes, a path class in range, no duplicate advert.
TEST(NodeRecord, RefusesWhatIsOutOfBounds) {
  const auto refused = [](const std::function<void(NodeRecord&)>& spoil) {
    auto node = sample();
    spoil(node);
    net::Writer writer;
    encode(writer, node);
    const auto bytes = writer.take();
    net::Reader reader(bytes);
    try {
      (void)decode(reader);
    } catch (const net::DecodeError&) {
      return true;
    }
    return false;
  };
  EXPECT_TRUE(refused([](NodeRecord& n) { n.candidates.assign(17, Candidate{"h", 1, false, 0}); }));
  EXPECT_FALSE(refused([](NodeRecord& n) { n.candidates.assign(16, Candidate{"h", 1, false, 0}); }));
  EXPECT_TRUE(refused([](NodeRecord& n) {
    for (int i = 0; i < 65; ++i) n.adverts["k" + std::to_string(i)] = {};
  }));
  EXPECT_TRUE(refused([](NodeRecord& n) { n.adverts["big"] = net::Bytes(1025, 0); }));
  EXPECT_TRUE(refused([](NodeRecord& n) { n.adverts[std::string(257, 'k')] = {}; }));
  EXPECT_TRUE(refused([](NodeRecord& n) { n.adverts[""] = {}; }));
  EXPECT_TRUE(refused([](NodeRecord& n) { n.path_class = static_cast<PathClass>(3); }));
}

// A plain node is dialable, relays for nobody, and is public until level 0
// learns otherwise.
TEST(NodeRecord, DefaultsToADialablePublicNode) {
  const NodeRecord node;
  EXPECT_TRUE(node.inbound_capable());
  EXPECT_FALSE(node.relays());
  EXPECT_EQ(node.path_class, PathClass::public_path);
  EXPECT_FALSE(node.advert("anything"));
  EXPECT_EQ(sample().advert_u64("capacity"), 0x0102030405060708ull);
  EXPECT_STREQ(path_class_name(PathClass::relay_only), "relay_only");
}

}  // namespace
}  // namespace dannan::membership
