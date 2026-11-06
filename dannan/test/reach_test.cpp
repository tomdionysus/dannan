//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Reachability v0: the inventory's "Public connectivity" table and the
// transport rows on `inbound_capable: auto` and an impossible cluster. The
// resolution by dial-back (persisted, sticky, re-probed by cadence), the
// dial-back probe's codec and its rate limit, public connectivity with its
// gateway and lookup injected, and the refusal of a cluster no node could
// reach.

#include <gtest/gtest.h>

#include <boost/asio/ip/tcp.hpp>
#include <fstream>
#include <future>
#include <thread>

#include "dannan/reach/connectivity.h"
#include "dannan/reach/dial_back.h"
#include "dannan/reach/inbound.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/recording_sink.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::reach {
namespace {

using namespace std::chrono_literals;

identity::NodeId id(uint8_t n) {
  identity::NodeId out;
  out.bytes[0] = n;
  return out;
}

// A configured value is final; auto starts capable, or from what was
// persisted, so a restart does not look like a join or a leave.
// A full disk costs the record, not the answer: Macha warns and carries on.
TEST(InboundResolution, AnAnswerThatCannotBePersistedStillHolds) {
  test::TempDir dir;
  test::SteppedWall wall;
  std::ofstream(dir.path() / "connectivity") << "a file where the directory goes";
  InboundResolver resolver(Inbound::automatic, dir.path(), &wall);
  EXPECT_TRUE(resolver.observe_probe(id(2), false, "refused").persist_error.empty());
  const auto verdict = resolver.observe_probe(id(2), false, "refused");
  EXPECT_FALSE(verdict.persist_error.empty());
  EXPECT_TRUE(verdict.changed);
  EXPECT_FALSE(resolver.capable());
}

TEST(InboundResolution, ConfiguredIsFinalAndAutoStartsFromWhatWasPersisted) {
  test::TempDir dir;
  test::SteppedWall wall;
  EXPECT_FALSE(InboundResolver(Inbound::no, dir.path(), &wall).capable());
  EXPECT_EQ(InboundResolver(Inbound::yes, dir.path(), &wall).state().source, "configured");
  {
    InboundResolver resolver(Inbound::automatic, dir.path(), &wall);
    EXPECT_TRUE(resolver.capable());
    EXPECT_EQ(resolver.state().source, "default");
    resolver.observe_probe(id(2), false, "refused");
    resolver.observe_probe(id(2), false, "refused");
    EXPECT_FALSE(resolver.capable());
  }
  InboundResolver reloaded(Inbound::automatic, dir.path(), &wall);
  EXPECT_FALSE(reloaded.capable());
  EXPECT_EQ(reloaded.state().source, "persisted");
  std::ofstream(dir.path() / "connectivity" / "inbound.bin", std::ios::binary) << "junk";
  InboundResolver corrupt(Inbound::automatic, dir.path(), &wall);
  EXPECT_TRUE(corrupt.capable());
  EXPECT_EQ(corrupt.state().source, "default");
}

// The persisted file: DNNINB01, the answer, when it was decided, by whom.
TEST(InboundResolution, PersistsItsFile) {
  test::TempDir dir;
  test::SteppedWall wall;
  InboundResolver resolver(Inbound::automatic, dir.path(), &wall);
  resolver.observe_probe(id(2), true, {});
  std::ifstream input(dir.path() / "connectivity" / "inbound.bin", std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  net::Writer expected;
  for (const char c : std::string("DNNINB01")) expected.u8(static_cast<uint8_t>(c));
  expected.u8(1);
  expected.u64(wall.unix_ms());
  expected.string("probe:" + to_string(id(2)));
  EXPECT_EQ(bytes, expected.data());
}

// Sticky: capable to incapable takes two failures in a row, back takes one
// success; the next probe follows the answer's cadence.
TEST(InboundResolution, IsStickyAndFollowsTheCadence) {
  test::TempDir dir;
  test::SteppedWall wall;
  InboundResolver resolver(Inbound::automatic, dir.path(), &wall);
  const auto first = resolver.observe_probe(id(2), false, "timeout");
  EXPECT_FALSE(first.changed);
  EXPECT_TRUE(resolver.capable());
  EXPECT_EQ(first.next_probe, InboundResolver::confirm_after);
  const auto second = resolver.observe_probe(id(3), false, "timeout");
  EXPECT_TRUE(second.changed);
  EXPECT_FALSE(resolver.capable());
  EXPECT_EQ(second.next_probe, 10min);
  EXPECT_EQ(resolver.state().consecutive_failures, 2u);
  EXPECT_EQ(resolver.state().last_probe_error, "timeout");
  const auto back = resolver.observe_probe(id(2), true, {});
  EXPECT_TRUE(back.changed);
  EXPECT_TRUE(resolver.capable());
  EXPECT_EQ(back.next_probe, 1h);
  EXPECT_EQ(resolver.state().consecutive_failures, 0u);
}

TEST(DialBack, RequestAndReplyEncodeAsProtocol23) {
  const auto request = encode_dial_back_request({"10.0.0.2", 7437});
  net::Writer expected;
  expected.string("10.0.0.2");
  expected.u16(7437);
  EXPECT_EQ(request, expected.data());
  const auto decoded = decode_dial_back_request(request);
  EXPECT_EQ(decoded.host, "10.0.0.2");
  EXPECT_EQ(decoded.port, 7437);
  const auto reply = encode_dial_back_reply(false, "refused");
  const auto [reachable, error] = decode_dial_back_reply(reply);
  EXPECT_FALSE(reachable);
  EXPECT_EQ(error, "refused");
}

// One probe per asking peer per interval, so the probe cannot be used to
// make this node hammer an address; a request without an address is
// refused.
TEST(DialBack, IsRateLimitedPerAskingPeer) {
  test::SteppedTime time;
  std::vector<net::Endpoint> probed;
  DialBackResponder responder(
      10s,
      [&](const net::Endpoint& endpoint, const identity::NodeId&) {
        probed.push_back(endpoint);
        return std::string();
      },
      &time);
  const auto request = encode_dial_back_request({"10.0.0.2", 7437});
  const auto first = responder.answer(id(2), request);
  ASSERT_EQ(first.outcome(), net::Handled::Outcome::processed);
  EXPECT_EQ(decode_dial_back_reply(first.reply().payload).first, true);
  EXPECT_EQ(responder.answer(id(2), request).outcome(), net::Handled::Outcome::failed);
  EXPECT_EQ(net::refusal_of(responder.answer(id(2), request).reply())->status, 429);
  EXPECT_EQ(responder.answer(id(3), request).outcome(), net::Handled::Outcome::processed);
  time.advance(10s);
  EXPECT_EQ(responder.answer(id(2), request).outcome(), net::Handled::Outcome::processed);
  EXPECT_EQ(probed.size(), 3u);
  EXPECT_EQ(net::refusal_of(responder.answer(id(4), encode_dial_back_request({"", 0})).reply())->status, 400);
}

// A gateway that does what a test says.
struct FakeGateway : Gateway {
  bool found = true;
  bool usable = true;
  std::string lan = "192.168.1.10";
  std::string wan = "203.0.113.5";
  std::optional<Mapping> existing;
  bool add_ok = true;
  int deletes = 0;
  int delete_code = 0;
  bool private_wan = false;
  std::optional<Session> discover(std::chrono::milliseconds, std::string& error) override {
    if (!found) {
      error = "no IGD";
      return std::nullopt;
    }
    return Session{usable, lan, wan, 1, private_wan};
  }
  std::optional<Mapping> mapping(uint16_t) override { return existing; }
  std::string add(uint16_t, uint16_t internal, const std::string& client, const std::string&, uint32_t) override {
    if (!add_ok) return "refused";
    existing = Mapping{client, internal};
    return {};
  }
  int remove(uint16_t) override {
    ++deletes;
    if (delete_code == 0) existing.reset();
    return delete_code;
  }
};

ConnectivityConfig upnp_on() {
  ConnectivityConfig config;
  config.upnp.enabled = true;
  return config;
}

// Status reads what is known: a probe in progress, however slow, delays no one.
TEST(PublicConnectivity, StatusNeverWaitsOnAProbe) {
  test::Gate gate;
  ConnectivityConfig config;
  config.check.enabled = true;
  PublicConnectivity connectivity(config, id(1), {"10.0.0.1", 7437}, nullptr, nullptr, [&](const net::Endpoint&, std::chrono::milliseconds, std::string&) {
    gate.enter_and_wait();
    return true;
  });
  std::thread prober([&] { (void)connectivity.refresh(true, true); });
  ASSERT_TRUE(gate.wait_for_entries(1));
  auto read = std::async(std::launch::async, [&] { return connectivity.status(); });
  EXPECT_EQ(read.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  gate.open();
  prober.join();
}

TEST(PublicConnectivity, WhatItFindsIsJournalled) {
  auto log = std::make_shared<test::RecordingSink>();
  auto gateway = std::make_shared<FakeGateway>();
  {
    auto config = upnp_on();
    config.check.enabled = true;
    PublicConnectivity connectivity(
        config, id(1), {"10.0.0.1", 7437}, gateway, nullptr,
        [](const net::Endpoint&, std::chrono::milliseconds, std::string& error) {
          error = "refused";
          return false;
        },
        &contract::system_wall_clock(), log);
    (void)connectivity.refresh(true);
    EXPECT_EQ(log->count(status::LogLevel::info, "UPnP port mapping active gateway_external=203.0.113.5:7437 internal="), 1u);
    EXPECT_EQ(
        log->count(
            status::LogLevel::warn,
            "connectivity self probe failed endpoint=203.0.113.5:7437 external_reachability=unknown reason=\"refused\" (NAT loopback may be unavailable)"),
        1u);
  }
  EXPECT_EQ(log->count(status::LogLevel::debug, "UPnP port mapping removed external_port=7437"), 1u);
}

TEST(PublicConnectivity, AnExternalIpIsJournalled) {
  auto log = std::make_shared<test::RecordingSink>();
  ConnectivityConfig config;
  config.external_ip.enabled = true;
  PublicConnectivity found(
      config, id(1), {"10.0.0.1", 7437}, nullptr, [](std::chrono::milliseconds) { return std::string("198.51.100.1"); }, nullptr,
      &contract::system_wall_clock(), log);
  (void)found.refresh(false);
  EXPECT_EQ(log->count(status::LogLevel::info, "external IP discovered source=aws address=198.51.100.1"), 1u);
  PublicConnectivity failing(
      config, id(1), {"10.0.0.1", 7437}, nullptr, [](std::chrono::milliseconds) -> std::string { throw std::runtime_error("no answer"); }, nullptr,
      &contract::system_wall_clock(), log);
  (void)failing.refresh(false);
  EXPECT_EQ(log->count(status::LogLevel::warn, "no answer"), 1u);
}

// A mapping made, verified and owned: the gateway's address is advertised;
// removed on exit because this node made it.
TEST(PublicConnectivity, AdvertisesAMappingItMadeAndRemovesItOnExit) {
  auto gateway = std::make_shared<FakeGateway>();
  {
    PublicConnectivity connectivity(upnp_on(), id(1), {"10.0.0.1", 7437}, gateway, nullptr, nullptr);
    const auto status = connectivity.refresh(false);
    EXPECT_TRUE(status.upnp.mapping_active);
    EXPECT_TRUE(status.upnp.mapping_owned);
    EXPECT_EQ(status.advertised.host, "203.0.113.5");
    EXPECT_EQ(status.advertised_source, "upnp");
  }
  EXPECT_EQ(gateway->deletes, 1);
}

TEST(PublicConnectivity, NamesWhyAMappingIsNotThere) {
  auto gateway = std::make_shared<FakeGateway>();
  gateway->existing = Gateway::Mapping{"192.168.1.99", 7437};
  PublicConnectivity elsewhere(upnp_on(), id(1), {"10.0.0.1", 7437}, gateway, nullptr, nullptr);
  auto status = elsewhere.refresh(false);
  EXPECT_EQ(status.upnp.error_code, "port_mapped_elsewhere");
  EXPECT_EQ(status.advertised_source, "configured");
  gateway->existing.reset();
  gateway->usable = false;
  EXPECT_EQ(elsewhere.refresh(false).upnp.error_code, "igd_not_connected");
  gateway->found = false;
  EXPECT_EQ(elsewhere.refresh(false).upnp.error_code, "discovery_failed");
  gateway->found = true;
  gateway->usable = true;
  gateway->add_ok = false;
  EXPECT_EQ(elsewhere.refresh(false).upnp.error_code, "add_mapping_failed");
}

// Without a usable mapping, the external address comes from the lookup.
TEST(PublicConnectivity, FallsBackToTheExternalAddress) {
  ConnectivityConfig config;
  config.external_ip.enabled = true;
  PublicConnectivity found(config, id(1), {"10.0.0.1", 7437}, nullptr, [](std::chrono::milliseconds) { return std::string("198.51.100.7"); }, nullptr);
  auto status = found.refresh(false);
  EXPECT_EQ(status.advertised.host, "198.51.100.7");
  EXPECT_EQ(status.advertised.port, 7437);
  EXPECT_EQ(status.advertised_source, "external_ip");
  PublicConnectivity failing(
      config, id(1), {"10.0.0.1", 7437}, nullptr, [](std::chrono::milliseconds) -> std::string { throw std::runtime_error("no route"); }, nullptr);
  status = failing.refresh(false);
  EXPECT_EQ(status.external_ip.error_code, "lookup_failed");
  EXPECT_EQ(status.advertised_source, "configured");
}

// The self-probe connects to the advertised endpoint; a disabled check
// says so unless forced.
TEST(PublicConnectivity, TheSelfProbeConnectsToTheAdvertisedEndpoint) {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor listening(io, {boost::asio::ip::address_v4::loopback(), 0});
  const auto port = listening.local_endpoint().port();
  ConnectivityConfig config;
  PublicConnectivity connectivity(config, id(1), {"127.0.0.1", port}, nullptr, nullptr, tcp_self_probe);
  EXPECT_EQ(connectivity.probe(false).self_probe, "disabled");
  EXPECT_EQ(connectivity.probe(true).self_probe, "reachable");
  listening.close();
  const auto closed = connectivity.probe(true);
  EXPECT_EQ(closed.self_probe, "unreachable");
  EXPECT_FALSE(closed.self_probe_error.empty());
}

membership::NodeRecord record(uint8_t n, const std::string& host, uint16_t port, bool capable) {
  membership::NodeRecord out;
  out.id = id(n);
  out.host = host;
  out.port = port;
  out.capabilities = capable ? membership::capability_inbound : 0;
  return out;
}

// A founding node must take inbound connections; and a node that takes none
// whose every bootstrap peer is known to take none could be reached by
// nobody.
TEST(ImpossibleCluster, IsRefusedWithAReason) {
  EXPECT_NO_THROW(refuse_impossible_cluster(Inbound::automatic, {}, {}, id(1)));
  try {
    refuse_impossible_cluster(Inbound::no, {}, {}, id(1));
    FAIL();
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("founding node"), std::string::npos);
  }
  const std::vector<net::Endpoint> bootstrap{{"10.0.0.2", 7437}};
  EXPECT_NO_THROW(refuse_impossible_cluster(Inbound::no, bootstrap, {}, id(1)));
  EXPECT_NO_THROW(refuse_impossible_cluster(Inbound::no, bootstrap, {record(2, "10.0.0.2", 7437, true)}, id(1)));
  EXPECT_THROW(refuse_impossible_cluster(Inbound::no, bootstrap, {record(2, "10.0.0.2", 7437, false)}, id(1)), std::runtime_error);
}

// The rest of the connectivity lines, level and words (first cutover, section 5).
TEST(PublicConnectivity, APrivateWanAddressIsWarnedOf) {
  auto log = std::make_shared<test::RecordingSink>();
  auto gateway = std::make_shared<FakeGateway>();
  gateway->wan = "100.64.0.5";
  gateway->private_wan = true;
  PublicConnectivity connectivity(upnp_on(), id(1), {"10.0.0.1", 7437}, gateway, nullptr, nullptr, &contract::system_wall_clock(), log);
  (void)connectivity.refresh(false);
  EXPECT_EQ(log->count(status::LogLevel::warn, "UPnP gateway WAN address is private address=100.64.0.5 probable_cgnat=true"), 1u);
}

TEST(PublicConnectivity, AMappingThatFailedIsWarnedOfInItsOwnWords) {
  auto log = std::make_shared<test::RecordingSink>();
  auto gateway = std::make_shared<FakeGateway>();
  gateway->add_ok = false;
  PublicConnectivity connectivity(upnp_on(), id(1), {"10.0.0.1", 7437}, gateway, nullptr, nullptr, &contract::system_wall_clock(), log);
  const auto status = connectivity.refresh(false);
  ASSERT_FALSE(status.upnp.error.empty());
  EXPECT_EQ(log->count(status::LogLevel::warn, status.upnp.error), 1u);
}

TEST(PublicConnectivity, AFailedExternalIpIsWarnedOfInItsOwnWords) {
  auto log = std::make_shared<test::RecordingSink>();
  ConnectivityConfig config;
  config.external_ip.enabled = true;
  PublicConnectivity connectivity(
      config, id(1), {"10.0.0.1", 7437}, nullptr, [](std::chrono::milliseconds) -> std::string { throw std::runtime_error("no answer"); }, nullptr,
      &contract::system_wall_clock(), log);
  const auto status = connectivity.refresh(false);
  ASSERT_FALSE(status.external_ip.error.empty());
  EXPECT_EQ(log->count(status::LogLevel::warn, status.external_ip.error), 1u);
}

TEST(PublicConnectivity, AReachableSelfProbeIsSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  ConnectivityConfig config;
  config.check.enabled = true;
  PublicConnectivity connectivity(
      config, id(1), {"10.0.0.1", 7437}, nullptr, nullptr, [](const net::Endpoint&, std::chrono::milliseconds, std::string&) { return true; },
      &contract::system_wall_clock(), log);
  (void)connectivity.refresh(true);
  EXPECT_EQ(log->count(status::LogLevel::info, "connectivity self probe reachable endpoint=10.0.0.1:7437 external_verification=unknown"), 1u);
}

TEST(PublicConnectivity, AMappingChangedSinceIsLeftAndSaid) {
  auto log = std::make_shared<test::RecordingSink>();
  auto gateway = std::make_shared<FakeGateway>();
  {
    PublicConnectivity connectivity(upnp_on(), id(1), {"10.0.0.1", 7437}, gateway, nullptr, nullptr, &contract::system_wall_clock(), log);
    (void)connectivity.refresh(false);
    gateway->existing = Gateway::Mapping{"192.168.1.99", 7437};
  }
  EXPECT_EQ(gateway->deletes, 0);
  EXPECT_EQ(log->count(status::LogLevel::debug, "UPnP owned mapping no longer matches; leaving external_port=7437"), 1u);
}

TEST(PublicConnectivity, ARemovalTheGatewayRefusesIsSaidWithItsCode) {
  auto log = std::make_shared<test::RecordingSink>();
  auto gateway = std::make_shared<FakeGateway>();
  gateway->delete_code = 714;
  {
    PublicConnectivity connectivity(upnp_on(), id(1), {"10.0.0.1", 7437}, gateway, nullptr, nullptr, &contract::system_wall_clock(), log);
    (void)connectivity.refresh(false);
  }
  EXPECT_EQ(log->count(status::LogLevel::debug, "UPnP port mapping removal failed code=714"), 1u);
  EXPECT_EQ(log->count(status::LogLevel::debug, "UPnP port mapping removed"), 0u);
}

}  // namespace
}  // namespace dannan::reach
