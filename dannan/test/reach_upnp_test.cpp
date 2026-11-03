//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The UPnP IGD client, written by hand: SSDP's answer, the device
// description, SOAP in and out, and the control path end to end against a
// fake gateway on loopback. And the external address lookup's answer. No
// case touches a real network.

#include <gtest/gtest.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <map>
#include <thread>

#include "dannan/reach/external_ip.h"
#include "dannan/reach/upnp.h"

namespace dannan::reach {
namespace {

namespace asio = boost::asio;

TEST(Upnp, ReadsTheLocationFromAnSsdpAnswer) {
  EXPECT_EQ(ssdp_location("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nlocation: http://192.168.1.1:5000/rootDesc.xml\r\nST: x\r\n\r\n"),
            "http://192.168.1.1:5000/rootDesc.xml");
  EXPECT_FALSE(ssdp_location("HTTP/1.1 200 OK\r\nST: x\r\n\r\n"));
}

TEST(Upnp, SplitsAUrl) {
  const auto url = parse_url("http://192.168.1.1:5000/ctl/IPConn");
  ASSERT_TRUE(url);
  EXPECT_EQ(url->host, "192.168.1.1");
  EXPECT_EQ(url->port, 5000);
  EXPECT_EQ(url->path, "/ctl/IPConn");
  EXPECT_EQ(parse_url("http://gw/")->port, 80);
  EXPECT_FALSE(parse_url("https://gw/"));
}

constexpr const char* description = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><device><deviceList><device><deviceList><device>
<serviceList>
<service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType><controlURL>/ctl/CmnIfCfg</controlURL></service>
<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl/IPConn</controlURL></service>
</serviceList></device></deviceList></device></deviceList></device></root>)";

TEST(Upnp, FindsTheConnectionServiceInTheDescription) {
  const auto found = connection_service(description, "http://192.168.1.1:5000/rootDesc.xml");
  ASSERT_TRUE(found);
  EXPECT_EQ(found->service_type, "urn:schemas-upnp-org:service:WANIPConnection:1");
  EXPECT_EQ(found->control_url, "http://192.168.1.1:5000/ctl/IPConn");
  EXPECT_FALSE(connection_service("<root/>", "http://x/"));
}

TEST(Upnp, ReadsSoapValuesAndFaults) {
  const auto body = soap_request("GetSpecificPortMappingEntry", "urn:x:1", {{"NewExternalPort", "7437"}, {"NewProtocol", "TCP"}});
  EXPECT_NE(body.find("<u:GetSpecificPortMappingEntry xmlns:u=\"urn:x:1\">"), std::string::npos);
  EXPECT_NE(body.find("<NewExternalPort>7437</NewExternalPort>"), std::string::npos);
  const auto values = soap_values(
      "<s:Envelope><s:Body><u:R><NewInternalClient>192.168.1.10</NewInternalClient><NewInternalPort>7437</NewInternalPort></u:R></s:Body></s:Envelope>");
  EXPECT_EQ(values.at("NewInternalClient"), "192.168.1.10");
  EXPECT_EQ(values.at("NewInternalPort"), "7437");
  EXPECT_TRUE(soap_fault("<s:Fault><detail><UPnPError><errorCode>714</errorCode></UPnPError></detail></s:Fault>"));
}

TEST(Upnp, KnowsAPrivateWan) {
  for (const auto* address : {"10.1.2.3", "172.16.0.1", "192.168.0.1", "100.64.0.1", "0.0.0.0"}) EXPECT_TRUE(private_ipv4(address)) << address;
  for (const auto* address : {"203.0.113.5", "8.8.8.8", "172.32.0.1"}) EXPECT_FALSE(private_ipv4(address)) << address;
}

// A gateway on loopback: serves the description and answers SOAP from a
// table of mappings.
struct FakeIgd {
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
  std::map<std::string, std::pair<std::string, std::string>> mappings;
  std::string status = "Connected";
  std::thread thread;
  std::atomic<bool> stopping{false};

  FakeIgd() {
    thread = std::thread([this] {
      while (!stopping) {
        asio::ip::tcp::socket socket(io);
        boost::system::error_code error;
        acceptor.accept(socket, error);
        if (error || stopping) return;
        serve(socket);
      }
    });
  }
  ~FakeIgd() {
    stopping = true;
    boost::system::error_code ignored;
    asio::ip::tcp::socket wake(io);
    wake.connect(acceptor.local_endpoint(), ignored);
    thread.join();
  }
  std::string location() const { return "http://127.0.0.1:" + std::to_string(acceptor.local_endpoint().port()) + "/rootDesc.xml"; }

  void serve(asio::ip::tcp::socket& socket) {
    asio::streambuf buffer;
    boost::system::error_code error;
    asio::read_until(socket, buffer, "\r\n\r\n", error);
    std::string head{asio::buffers_begin(buffer.data()), asio::buffers_end(buffer.data())};
    const auto end = head.find("\r\n\r\n");
    std::string body = head.substr(end + 4);
    head.resize(end);
    size_t length = 0;
    if (const auto at = head.find("Content-Length: "); at != std::string::npos) length = std::stoul(head.substr(at + 16));
    while (body.size() < length) {
      char chunk[1024];
      const auto got = socket.read_some(asio::buffer(chunk), error);
      if (error) break;
      body.append(chunk, got);
    }
    std::string reply;
    if (head.starts_with("GET")) {
      reply = description;
    } else {
      const auto value = [&](const std::string& name) {
        const auto open = body.find("<" + name + ">");
        const auto close = body.find("</" + name + ">");
        return open == std::string::npos ? std::string() : body.substr(open + name.size() + 2, close - open - name.size() - 2);
      };
      if (head.find("#GetStatusInfo") != std::string::npos) {
        reply = "<s:Envelope><s:Body><u:R><NewConnectionStatus>" + status + "</NewConnectionStatus></u:R></s:Body></s:Envelope>";
      } else if (head.find("#GetExternalIPAddress") != std::string::npos) {
        reply = "<s:Envelope><s:Body><u:R><NewExternalIPAddress>203.0.113.5</NewExternalIPAddress></u:R></s:Body></s:Envelope>";
      } else if (head.find("#GetSpecificPortMappingEntry") != std::string::npos) {
        const auto found = mappings.find(value("NewExternalPort"));
        reply = found == mappings.end()
                    ? "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>714</errorCode></UPnPError></detail></s:Fault></s:Body></s:Envelope>"
                    : "<s:Envelope><s:Body><u:R><NewInternalPort>" + found->second.second + "</NewInternalPort><NewInternalClient>" + found->second.first +
                          "</NewInternalClient></u:R></s:Body></s:Envelope>";
      } else if (head.find("#AddPortMapping") != std::string::npos) {
        mappings[value("NewExternalPort")] = {value("NewInternalClient"), value("NewInternalPort")};
        reply = "<s:Envelope><s:Body><u:R/></s:Body></s:Envelope>";
      } else if (head.find("#DeletePortMapping") != std::string::npos) {
        mappings.erase(value("NewExternalPort"));
        reply = "<s:Envelope><s:Body><u:R/></s:Body></s:Envelope>";
      }
    }
    const auto response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " + std::to_string(reply.size()) + "\r\nConnection: close\r\n\r\n" + reply;
    asio::write(socket, asio::buffer(response), error);
  }
};

TEST(Upnp, MapsAPortThroughAGateway) {
  FakeIgd igd;
  UpnpGateway gateway({igd.location()});
  std::string error;
  const auto session = gateway.discover(std::chrono::seconds(2), error);
  ASSERT_TRUE(session) << error;
  EXPECT_TRUE(session->usable);
  EXPECT_EQ(session->wan_address, "203.0.113.5");
  EXPECT_FALSE(session->private_wan);
  EXPECT_EQ(session->lan_address, "127.0.0.1");
  EXPECT_FALSE(gateway.mapping(7437));
  EXPECT_EQ(gateway.add(7437, 7437, session->lan_address, "Macha test", 0), "");
  const auto made = gateway.mapping(7437);
  ASSERT_TRUE(made);
  EXPECT_EQ(made->internal_client, "127.0.0.1");
  EXPECT_EQ(made->internal_port, 7437);
  gateway.remove(7437);
  EXPECT_FALSE(gateway.mapping(7437));
  igd.status = "Disconnected";
  EXPECT_FALSE(gateway.discover(std::chrono::seconds(2), error)->usable);
}

TEST(Upnp, ADiscoveryThatFindsNothingSaysSo) {
  UpnpGateway gateway({"http://127.0.0.1:1/rootDesc.xml"});
  std::string error;
  EXPECT_FALSE(gateway.discover(std::chrono::milliseconds(300), error));
  EXPECT_FALSE(error.empty());
}

TEST(ExternalIp, AcceptsOnlyAnIpv4Literal) {
  EXPECT_EQ(external_ip_from_body("  198.51.100.7\n"), "198.51.100.7");
  EXPECT_THROW(external_ip_from_body("<html>"), std::runtime_error);
  EXPECT_THROW(external_ip_from_body("2001:db8::1"), std::runtime_error);
}

}  // namespace
}  // namespace dannan::reach
