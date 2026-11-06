//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dannan/reach/connectivity.h"

// A UPnP Internet Gateway Device client, written by hand: SSDP discovery,
// the device description, and the WAN connection service's SOAP actions.
// Enough to make, check and remove one TCP port mapping; nothing more.
namespace dannan::reach {

struct Url {
  std::string host;
  uint16_t port = 80;
  std::string path = "/";
};

// The LOCATION header of an SSDP answer, any case.
std::optional<std::string> ssdp_location(std::string_view answer);
// An http:// URL; nothing for any other scheme.
std::optional<Url> parse_url(std::string_view url);

struct ConnectionService {
  std::string service_type;
  std::string control_url;
};
// The WANIPConnection (preferred) or WANPPPConnection service in a device
// description, its control URL made absolute against `location`.
std::optional<ConnectionService> connection_service(std::string_view description, std::string_view location);

std::string soap_request(std::string_view action, std::string_view service_type, const std::vector<std::pair<std::string, std::string>>& arguments);
// Every NewXxx element of a SOAP response, unescaped.
std::map<std::string, std::string> soap_values(std::string_view response);
bool soap_fault(std::string_view response);

// RFC 1918, carrier-grade NAT (100.64/10) and the unspecified address.
bool private_ipv4(const std::string& address);

// A Gateway over UPnP. `locations` given: their descriptions are read
// instead of searching by SSDP (a known gateway, or a test). Not
// thread-safe: PublicConnectivity holds its work lock across every call.
class UpnpGateway final : public Gateway {
 public:
  explicit UpnpGateway(std::vector<std::string> locations = {}) : _locations(std::move(locations)) {}

  std::optional<Session> discover(std::chrono::milliseconds timeout, std::string& error) override;
  std::optional<Mapping> mapping(uint16_t external_port) override;
  std::string add(uint16_t external_port, uint16_t internal_port, const std::string& internal_client, const std::string& description,
                  uint32_t lease_seconds) override;
  int remove(uint16_t external_port) override;

 private:
  // The response body, or nothing with `error` set; a SOAP fault is a body.
  std::optional<std::string> call(std::string_view action, const std::vector<std::pair<std::string, std::string>>& arguments, std::string& error);

  const std::vector<std::string> _locations;
  std::optional<ConnectionService> _service;
  std::chrono::milliseconds _timeout{2000};
};

}  // namespace dannan::reach
