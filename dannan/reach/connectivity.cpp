//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/reach/connectivity.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace dannan::reach {
namespace {

bool ipv4_literal(const std::string& value) {
  in_addr address{};
  return ::inet_pton(AF_INET, value.c_str(), &address) == 1;
}

}  // namespace

bool tcp_self_probe(const net::Endpoint& endpoint, std::chrono::milliseconds timeout, std::string& error) {
  if (endpoint.host.empty() || !endpoint.port) {
    error = "advertised endpoint is incomplete";
    return false;
  }
  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo* found = nullptr;
  const auto service = std::to_string(endpoint.port);
  if (const int resolved = ::getaddrinfo(endpoint.host.c_str(), service.c_str(), &hints, &found); resolved != 0) {
    error = std::string("resolve failed: ") + ::gai_strerror(resolved);
    return false;
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> addresses(found, ::freeaddrinfo);
  const auto deadline = contract::Clock::now() + timeout;
  std::string last = "connection failed";
  for (auto* address = addresses.get(); address; address = address->ai_next) {
    if (contract::Clock::now() >= deadline) break;
    const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      last = std::strerror(errno);
      continue;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      last = std::strerror(errno);
      ::close(fd);
      continue;
    }
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
      ::close(fd);
      return true;
    }
    if (errno != EINPROGRESS) {
      last = std::strerror(errno);
      ::close(fd);
      continue;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - contract::Clock::now()).count();
    pollfd item{fd, POLLOUT, 0};
    int ready;
    do {
      ready = ::poll(&item, 1, static_cast<int>(std::max<int64_t>(1, left)));
    } while (ready < 0 && errno == EINTR);
    if (ready > 0) {
      int socket_error = 0;
      socklen_t length = sizeof(socket_error);
      if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error == 0) {
        ::close(fd);
        return true;
      }
      last = socket_error ? std::strerror(socket_error) : "connect failed";
    } else {
      last = ready == 0 ? "timeout" : std::strerror(errno);
    }
    ::close(fd);
  }
  error = last;
  return false;
}

PublicConnectivity::PublicConnectivity(ConnectivityConfig config, identity::NodeId node, net::Endpoint configured, std::shared_ptr<Gateway> gateway,
                                       ExternalIpLookup lookup, SelfProbe probe, const contract::WallClock* wall, std::shared_ptr<status::LogSink> log)
    : _config(config),
      _node(node),
      _configured(std::move(configured)),
      _gateway(std::move(gateway)),
      _lookup(std::move(lookup)),
      _probe(std::move(probe)),
      _wall(wall),
      _log(log ? std::move(log) : status::null_sink(), "connectivity") {
  contract::Lock lock(_mutex);
  _status.configured = _configured;
  _status.advertised = _configured;
  _status.upnp.enabled = _config.upnp.enabled;
  _status.upnp.support_built = _gateway != nullptr;
  _status.upnp.internal_port = _configured.port;
  _status.upnp.external_port = _config.upnp.external_port ? _config.upnp.external_port : _configured.port;
  _status.upnp.lease_seconds = _config.upnp.lease_seconds;
  _status.external_ip.enabled = _config.external_ip.enabled;
  _status.check_enabled = _config.check.enabled;
}

PublicConnectivity::~PublicConnectivity() {
  contract::Lock work(_work_mutex);
  remove_owned_locked();
}

PublicConnectivityStatus PublicConnectivity::status() const {
  contract::Lock lock(_mutex);
  return _status;
}

PublicConnectivityStatus PublicConnectivity::refresh(bool probe, bool force_probe) {
  contract::Lock work(_work_mutex);
  auto next = status();
  refresh_locked(next);
  if (probe) probe_locked(next, force_probe);
  contract::Lock lock(_mutex);
  _status = next;
  return next;
}

PublicConnectivityStatus PublicConnectivity::probe(bool force) {
  contract::Lock work(_work_mutex);
  auto next = status();
  probe_locked(next, force);
  contract::Lock lock(_mutex);
  _status = next;
  return next;
}

void PublicConnectivity::refresh_locked(PublicConnectivityStatus& status) {
  status.advertised = _configured;
  status.advertised_source = "configured";
  auto& upnp = status.upnp;
  upnp.gateway_found = upnp.mapping_active = upnp.mapping_created = upnp.mapping_owned = upnp.private_wan = false;
  upnp.lan_address.clear();
  upnp.external_address.clear();
  upnp.igd_status = 0;
  upnp.error.clear();
  upnp.error_code.clear();
  status.external_ip.attempted = false;
  status.external_ip.address.clear();
  status.external_ip.error.clear();
  status.external_ip.error_code.clear();
  status.self_probe = "not_run";
  status.self_probe_error.clear();

  const uint16_t external_port = _config.upnp.external_port ? _config.upnp.external_port : _configured.port;
  if (_config.upnp.enabled && !_gateway) {
    upnp.error_code = "support_not_built";
    upnp.error = "UPnP support was not built";
  } else if (_config.upnp.enabled) {
    std::string discovery_error;
    if (const auto session = _gateway->discover(_config.upnp.discovery_timeout, discovery_error)) {
      upnp.gateway_found = true;
      upnp.igd_status = session->igd_status;
      upnp.lan_address = session->lan_address;
      upnp.external_address = session->wan_address;
      upnp.private_wan = session->private_wan;
      if (!session->usable) {
        upnp.error_code = "igd_not_connected";
        upnp.error = "UPnP IGD is not connected status=" + std::to_string(session->igd_status);
      } else {
        const auto existing = _gateway->mapping(external_port);
        bool active = existing && existing->internal_port == _configured.port && existing->internal_client == upnp.lan_address;
        if (existing && !active) {
          upnp.error_code = "port_mapped_elsewhere";
          upnp.error = "UPnP external port " + std::to_string(external_port) + " is already mapped to " + existing->internal_client + ":" +
                       std::to_string(existing->internal_port);
        } else if (!active) {
          const auto description = "Dannan " + to_string(_node).substr(0, 12);
          if (const auto failed = _gateway->add(external_port, _configured.port, upnp.lan_address, description, _config.upnp.lease_seconds); failed.empty()) {
            const auto made = _gateway->mapping(external_port);
            active = made && made->internal_port == _configured.port && made->internal_client == upnp.lan_address;
            if (active) {
              _owned = true;
              _owned_port = external_port;
              upnp.mapping_created = true;
            } else {
              upnp.error_code = "mapping_verification_failed";
              upnp.error = "UPnP AddPortMapping succeeded but mapping verification failed";
            }
          } else {
            upnp.error_code = "add_mapping_failed";
            upnp.error = "UPnP AddPortMapping failed: " + failed;
          }
        }
        upnp.mapping_active = active;
        upnp.mapping_owned = active && _owned;
        if (active && ipv4_literal(upnp.external_address)) {
          status.advertised = {upnp.external_address, external_port};
          status.advertised_source = "upnp";
        }
      }
    } else {
      upnp.error_code = "discovery_failed";
      upnp.error = discovery_error;
    }
  }

  const bool needs_lookup = _config.external_ip.enabled &&
                            (status.advertised_source == "configured" || upnp.private_wan || (upnp.mapping_active && !ipv4_literal(upnp.external_address)));
  if (needs_lookup) {
    status.external_ip.attempted = true;
    try {
      if (!_lookup) throw std::runtime_error("external IP lookup was not built");
      status.external_ip.address = _lookup(_config.external_ip.timeout);
      status.advertised.host = status.external_ip.address;
      status.advertised.port = upnp.mapping_active ? external_port : _configured.port;
      status.advertised_source = upnp.mapping_active ? "upnp+external_ip" : "external_ip";
    } catch (const std::exception& error) {
      status.external_ip.error_code = "lookup_failed";
      status.external_ip.error = error.what();
    }
  }
  status.checked_unix_ms = _wall->unix_ms();
  if (_config.upnp.enabled) {
    if (upnp.mapping_active) {
      _log.info("UPnP port mapping active gateway_external=" + upnp.external_address + ":" + std::to_string(external_port) + " internal=" + upnp.lan_address +
                ":" + std::to_string(_configured.port) + " lease_s=" + std::to_string(_config.upnp.lease_seconds));
      if (upnp.private_wan) _log.warn("UPnP gateway WAN address is private address=" + upnp.external_address + " probable_cgnat=true");
    } else if (!upnp.error.empty()) {
      _log.warn(upnp.error);
    }
  }
  if (status.external_ip.attempted) {
    if (!status.external_ip.address.empty())
      _log.info("external IP discovered source=aws address=" + status.external_ip.address);
    else if (!status.external_ip.error.empty())
      _log.warn(status.external_ip.error);
  }
}

void PublicConnectivity::probe_locked(PublicConnectivityStatus& status, bool force) {
  status.self_probe_error.clear();
  if (!_config.check.enabled && !force) {
    status.self_probe = "disabled";
  } else if (!_probe) {
    status.self_probe = "unreachable";
    status.self_probe_error = "self probe was not built";
  } else {
    std::string error;
    if (_probe(status.advertised, _config.check.timeout, error)) {
      status.self_probe = "reachable";
      _log.info("connectivity self probe reachable endpoint=" + status.advertised.host + ":" + std::to_string(status.advertised.port) +
                " external_verification=unknown");
    } else {
      status.self_probe = "unreachable";
      status.self_probe_error = error;
      _log.warn("connectivity self probe failed endpoint=" + status.advertised.host + ":" + std::to_string(status.advertised.port) +
                " external_reachability=unknown reason=\"" + error + "\" (NAT loopback may be unavailable)");
    }
  }
  status.checked_unix_ms = _wall->unix_ms();
}

void PublicConnectivity::remove_owned_locked() noexcept {
  if (!_owned || !_owned_port || !_gateway) return;
  try {
    std::string error;
    const auto session = _gateway->discover(_config.upnp.discovery_timeout, error);
    // Still ours: the next attempt may find the gateway.
    if (!session) return;
    const auto existing = _gateway->mapping(_owned_port);
    // Only the mapping this node made: one changed since is not ours.
    if (existing && existing->internal_port == _configured.port && existing->internal_client == session->lan_address) {
      const auto code = _gateway->remove(_owned_port);
      if (code == 0)
        _log.debug("UPnP port mapping removed external_port=" + std::to_string(_owned_port));
      else
        _log.debug("UPnP port mapping removal failed code=" + std::to_string(code));
    } else {
      _log.debug("UPnP owned mapping no longer matches; leaving external_port=" + std::to_string(_owned_port));
    }
  } catch (...) {
  }
  _owned = false;
  _owned_port = 0;
}

}  // namespace dannan::reach
