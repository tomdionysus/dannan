//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/reach/upnp.h"

#include <arpa/inet.h>

#include <algorithm>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>

namespace dannan::reach {
namespace {

namespace asio = boost::asio;

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string xml_escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

std::string xml_unescape(std::string text) {
  for (const auto& [from, to] :
       std::vector<std::pair<std::string, std::string>>{{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}}) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
  }
  return text;
}

// The text of the first <tag> inside `xml`, any namespace prefix.
std::optional<std::string> element(std::string_view xml, const std::string& tag) {
  const std::regex pattern("<(?:[A-Za-z0-9_]+:)?" + tag + R"((?:\s[^>]*)?>([^<]*)</(?:[A-Za-z0-9_]+:)?)" + tag + ">");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(xml.begin(), xml.end(), match, pattern)) return std::nullopt;
  std::string value = match[1].str();
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
  value.erase(0, std::min(value.find_first_not_of(" \t\r\n"), value.size()));
  return xml_unescape(value);
}

struct Response {
  int status = 0;
  std::string body;
  std::string local_address;
};

std::string decode_chunked(const std::string& body) {
  std::string out;
  size_t at = 0;
  while (at < body.size()) {
    const auto line_end = body.find("\r\n", at);
    if (line_end == std::string::npos) break;
    const auto size = std::stoul(body.substr(at, line_end - at), nullptr, 16);
    if (!size) break;
    out += body.substr(line_end + 2, size);
    at = line_end + 2 + size + 2;
  }
  return out;
}

// One HTTP/1.1 exchange, Connection: close, bounded by `timeout`.
std::optional<Response> http(const Url& url, std::string_view method, const std::vector<std::string>& headers, std::string_view body,
                             std::chrono::milliseconds timeout, std::string& error) {
  asio::ip::tcp::iostream stream;
  stream.expires_after(timeout);
  stream.connect(url.host, std::to_string(url.port));
  if (!stream) {
    error = "cannot reach " + url.host + ":" + std::to_string(url.port) + ": " + stream.error().message();
    return std::nullopt;
  }
  Response out;
  boost::system::error_code ignored;
  out.local_address = stream.socket().local_endpoint(ignored).address().to_string();
  stream << method << " " << url.path << " HTTP/1.1\r\nHost: " << url.host << ":" << url.port << "\r\nConnection: close\r\n";
  for (const auto& header : headers) stream << header << "\r\n";
  if (!body.empty() || method == "POST") stream << "Content-Length: " << body.size() << "\r\n";
  stream << "\r\n" << body;
  stream.flush();
  std::string status_line;
  if (!std::getline(stream, status_line)) {
    error = "no answer from " + url.host + ": " + stream.error().message();
    return std::nullopt;
  }
  std::istringstream status(status_line);
  std::string version;
  status >> version >> out.status;
  bool chunked = false;
  std::optional<size_t> length;
  for (std::string line; std::getline(stream, line) && line != "\r" && !line.empty();) {
    const auto header = lower(line);
    if (header.starts_with("content-length:")) length = std::stoul(header.substr(15));
    if (header.starts_with("transfer-encoding:") && header.find("chunked") != std::string::npos) chunked = true;
  }
  if (length && !chunked) {
    out.body.resize(*length);
    stream.read(out.body.data(), static_cast<std::streamsize>(*length));
    out.body.resize(static_cast<size_t>(stream.gcount()));
  } else {
    out.body.assign(std::istreambuf_iterator<char>(stream), {});
    if (chunked) out.body = decode_chunked(out.body);
  }
  return out;
}

// SSDP: an M-SEARCH for an IGD, and every LOCATION answered within the
// timeout.
std::vector<std::string> search(std::chrono::milliseconds timeout, std::string& error) {
  asio::io_context io;
  asio::ip::udp::socket socket(io, asio::ip::udp::v4());
  const asio::ip::udp::endpoint group(asio::ip::make_address("239.255.255.250"), 1900);
  boost::system::error_code sent;
  for (const auto* target : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
    const std::string request =
        std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + target + "\r\n\r\n";
    socket.send_to(asio::buffer(request), group, 0, sent);
  }
  if (sent) {
    error = "SSDP search failed: " + sent.message();
    return {};
  }
  std::vector<std::string> found;
  std::array<char, 2048> buffer{};
  asio::ip::udp::endpoint from;
  std::function<void(const boost::system::error_code&, size_t)> receive = [&](const boost::system::error_code& failed, size_t size) {
    if (failed) return;
    if (auto location = ssdp_location(std::string_view(buffer.data(), size)); location && std::find(found.begin(), found.end(), *location) == found.end())
      found.push_back(*location);
    socket.async_receive_from(asio::buffer(buffer), from, receive);
  };
  socket.async_receive_from(asio::buffer(buffer), from, receive);
  io.run_for(timeout);
  if (found.empty()) error = "no UPnP gateway answered";
  return found;
}

}  // namespace

std::optional<std::string> ssdp_location(std::string_view answer) {
  std::istringstream lines{std::string(answer)};
  for (std::string line; std::getline(lines, line);) {
    if (!lower(line).starts_with("location:")) continue;
    auto value = line.substr(9);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    value.erase(0, std::min(value.find_first_not_of(' '), value.size()));
    return value;
  }
  return std::nullopt;
}

std::optional<Url> parse_url(std::string_view text) {
  if (!text.starts_with("http://")) return std::nullopt;
  text.remove_prefix(7);
  const auto slash = text.find('/');
  const auto authority = text.substr(0, slash);
  Url out;
  out.path = slash == std::string_view::npos ? "/" : std::string(text.substr(slash));
  const auto colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    out.host = std::string(authority);
  } else {
    out.host = std::string(authority.substr(0, colon));
    out.port = static_cast<uint16_t>(std::stoi(std::string(authority.substr(colon + 1))));
  }
  if (out.host.empty()) return std::nullopt;
  return out;
}

std::optional<ConnectionService> connection_service(std::string_view description, std::string_view location) {
  std::optional<ConnectionService> ppp;
  std::optional<ConnectionService> ip;
  const std::regex service("<(?:[A-Za-z0-9_]+:)?service>([\\s\\S]*?)</(?:[A-Za-z0-9_]+:)?service>");
  for (auto it = std::regex_iterator<std::string_view::const_iterator>(description.begin(), description.end(), service); it != decltype(it){}; ++it) {
    const auto block = (*it)[1].str();
    const auto type = element(block, "serviceType");
    const auto control = element(block, "controlURL");
    if (!type || !control) continue;
    if (type->find(":WANIPConnection:") != std::string::npos && !ip) ip = ConnectionService{*type, *control};
    if (type->find(":WANPPPConnection:") != std::string::npos && !ppp) ppp = ConnectionService{*type, *control};
  }
  auto found = ip ? ip : ppp;
  if (!found) return std::nullopt;
  if (!found->control_url.starts_with("http://")) {
    std::string base = element(description, "URLBase").value_or(std::string(location));
    const auto host_end = base.find('/', 7);
    base = base.substr(0, host_end);
    if (!found->control_url.starts_with("/")) found->control_url.insert(0, "/");
    found->control_url = base + found->control_url;
  }
  return found;
}

std::string soap_request(std::string_view action, std::string_view service_type, const std::vector<std::pair<std::string, std::string>>& arguments) {
  std::string out =
      "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
  out += std::string(action) + " xmlns:u=\"" + std::string(service_type) + "\">";
  for (const auto& [name, value] : arguments) out += "<" + name + ">" + xml_escape(value) + "</" + name + ">";
  out += "</u:" + std::string(action) + "></s:Body></s:Envelope>\r\n";
  return out;
}

std::map<std::string, std::string> soap_values(std::string_view response) {
  std::map<std::string, std::string> out;
  const std::regex value("<(?:[A-Za-z0-9_]+:)?(New[A-Za-z0-9_]*)>([^<]*)</(?:[A-Za-z0-9_]+:)?\\1>");
  for (auto it = std::regex_iterator<std::string_view::const_iterator>(response.begin(), response.end(), value); it != decltype(it){}; ++it)
    out[(*it)[1].str()] = xml_unescape((*it)[2].str());
  return out;
}

bool soap_fault(std::string_view response) { return response.find("Fault>") != std::string_view::npos; }

bool private_ipv4(const std::string& address) {
  in_addr parsed{};
  if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1) return false;
  const uint32_t a = ntohl(parsed.s_addr);
  return (a >> 24) == 10 || (a >> 20) == (172u << 4 | 1) || (a >> 16) == (192u << 8 | 168) || (a >> 22) == (100u << 2 | 1) || a == 0;
}

std::optional<Gateway::Session> UpnpGateway::discover(std::chrono::milliseconds timeout, std::string& error) {
  _timeout = timeout;
  _service.reset();
  const auto locations = _locations.empty() ? search(timeout, error) : _locations;
  for (const auto& location : locations) {
    const auto url = parse_url(location);
    if (!url) continue;
    const auto description = http(*url, "GET", {}, {}, timeout, error);
    if (!description || description->status != 200) {
      if (description) error = "gateway description answered HTTP " + std::to_string(description->status);
      continue;
    }
    _service = connection_service(description->body, location);
    if (!_service) {
      error = "no WAN connection service at " + location;
      continue;
    }
    Session session;
    session.lan_address = description->local_address;
    const auto status = call("GetStatusInfo", {}, error);
    session.usable = status && soap_values(*status)["NewConnectionStatus"] == "Connected";
    if (const auto wan = call("GetExternalIPAddress", {}, error)) session.wan_address = soap_values(*wan)["NewExternalIPAddress"];
    session.private_wan = private_ipv4(session.wan_address);
    session.igd_status = !session.usable ? 3 : session.private_wan ? 2 : 1;
    return session;
  }
  if (error.empty()) error = "no UPnP gateway found";
  return std::nullopt;
}

std::optional<std::string> UpnpGateway::call(std::string_view action, const std::vector<std::pair<std::string, std::string>>& arguments, std::string& error) {
  if (!_service) {
    error = "no gateway discovered";
    return std::nullopt;
  }
  const auto url = parse_url(_service->control_url);
  if (!url) {
    error = "unusable control URL " + _service->control_url;
    return std::nullopt;
  }
  const auto response =
      http(*url, "POST", {"Content-Type: text/xml; charset=\"utf-8\"", "SOAPAction: \"" + _service->service_type + "#" + std::string(action) + "\""},
           soap_request(action, _service->service_type, arguments), _timeout, error);
  if (!response) return std::nullopt;
  return response->body;
}

std::optional<Gateway::Mapping> UpnpGateway::mapping(uint16_t external_port) {
  std::string error;
  const auto response =
      call("GetSpecificPortMappingEntry", {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(external_port)}, {"NewProtocol", "TCP"}}, error);
  if (!response || soap_fault(*response)) return std::nullopt;
  auto values = soap_values(*response);
  Mapping out;
  out.internal_client = values["NewInternalClient"];
  try {
    const auto port = std::stoul(values["NewInternalPort"]);
    out.internal_port = port <= 65535 ? static_cast<uint16_t>(port) : 0;
  } catch (const std::exception&) {
    out.internal_port = 0;
  }
  return out;
}

std::string UpnpGateway::add(uint16_t external_port, uint16_t internal_port, const std::string& internal_client, const std::string& description,
                             uint32_t lease_seconds) {
  std::string error;
  const auto response = call("AddPortMapping",
                             {{"NewRemoteHost", ""},
                              {"NewExternalPort", std::to_string(external_port)},
                              {"NewProtocol", "TCP"},
                              {"NewInternalPort", std::to_string(internal_port)},
                              {"NewInternalClient", internal_client},
                              {"NewEnabled", "1"},
                              {"NewPortMappingDescription", description},
                              {"NewLeaseDuration", std::to_string(lease_seconds)}},
                             error);
  if (!response) return error;
  if (soap_fault(*response)) {
    const auto code = element(*response, "errorCode").value_or("unknown");
    return "the gateway refused, UPnP error " + code;
  }
  return {};
}

void UpnpGateway::remove(uint16_t external_port) {
  std::string error;
  (void)call("DeletePortMapping", {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(external_port)}, {"NewProtocol", "TCP"}}, error);
}

}  // namespace dannan::reach
