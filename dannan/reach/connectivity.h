//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/identity/node_id.h"
#include "dannan/net/client.h"

namespace dannan::reach {

struct ConnectivityConfig {
  struct Upnp {
    bool enabled = false;
    // Zero: the same as the internal port.
    uint16_t external_port = 0;
    std::chrono::milliseconds discovery_timeout{2000};
    uint32_t lease_seconds = 0;
  } upnp;
  struct ExternalIp {
    bool enabled = false;
    std::chrono::milliseconds timeout{3000};
  } external_ip;
  struct Check {
    bool enabled = false;
    std::chrono::milliseconds timeout{3000};
  } check;
};

// A NAT gateway's port mapping service (UPnP IGD), behind an interface so a
// node is tested against a fake. Implementations may wait on the network
// within the timeouts they are given; never called concurrently.
class Gateway {
 public:
  struct Session {
    bool usable = false;  // connected
    std::string lan_address;
    std::string wan_address;
    int igd_status = 0;
    // The WAN address is private: probably carrier-grade NAT.
    bool private_wan = false;
  };
  struct Mapping {
    std::string internal_client;
    uint16_t internal_port = 0;
  };

  virtual ~Gateway() = default;
  virtual std::optional<Session> discover(std::chrono::milliseconds timeout, std::string& error) = 0;
  virtual std::optional<Mapping> mapping(uint16_t external_port) = 0;
  // The reason it failed; empty on success.
  virtual std::string add(uint16_t external_port, uint16_t internal_port, const std::string& internal_client, const std::string& description,
                          uint32_t lease_seconds) = 0;
  virtual void remove(uint16_t external_port) = 0;
};

// This node's address as the internet sees it; throws with the reason.
using ExternalIpLookup = std::function<std::string(std::chrono::milliseconds timeout)>;
// Whether a connection to `endpoint` completes within `timeout`; `error`
// says why not.
using SelfProbe = std::function<bool(const net::Endpoint& endpoint, std::chrono::milliseconds timeout, std::string& error)>;

// A TCP connect to the endpoint, every resolved address in turn, within the
// timeout.
bool tcp_self_probe(const net::Endpoint& endpoint, std::chrono::milliseconds timeout, std::string& error);

struct UpnpStatus {
  bool enabled = false;
  bool support_built = false;
  bool gateway_found = false;
  bool mapping_active = false;
  bool mapping_created = false;
  bool mapping_owned = false;
  bool private_wan = false;
  std::string lan_address;
  std::string external_address;
  uint16_t internal_port = 0;
  uint16_t external_port = 0;
  uint32_t lease_seconds = 0;
  int igd_status = 0;
  // igd_not_connected, port_mapped_elsewhere, mapping_verification_failed,
  // add_mapping_failed, discovery_failed, support_not_built; `error` is the
  // English beside it.
  std::string error_code;
  std::string error;
};

struct ExternalIpStatus {
  bool enabled = false;
  bool attempted = false;
  std::string address;
  std::string error_code;  // lookup_failed
  std::string error;
};

struct PublicConnectivityStatus {
  net::Endpoint configured;
  net::Endpoint advertised;
  // configured, upnp, external_ip, upnp+external_ip.
  std::string advertised_source = "configured";
  UpnpStatus upnp;
  ExternalIpStatus external_ip;
  bool check_enabled = false;
  // not_run, disabled, reachable, unreachable.
  std::string self_probe = "not_run";
  std::string self_probe_error;
  uint64_t checked_unix_ms = 0;
};

// What this node advertises and how it got there: a UPnP mapping it made,
// verified and owns (removed on exit), else the external address looked up,
// else the configured endpoint; and a self-probe of the result. One check
// at a time; status() never waits on one (the work lock is taken before the
// status lock, never inside it). A null gateway, lookup or probe is a
// feature not built.
class PublicConnectivity {
 public:
  PublicConnectivity(ConnectivityConfig config, identity::NodeId node, net::Endpoint configured, std::shared_ptr<Gateway> gateway, ExternalIpLookup lookup,
                     SelfProbe probe, const contract::WallClock* wall = &contract::system_wall_clock());
  ~PublicConnectivity();
  PublicConnectivity(const PublicConnectivity&) = delete;
  PublicConnectivity& operator=(const PublicConnectivity&) = delete;

  PublicConnectivityStatus refresh(bool probe, bool force_probe = false);
  PublicConnectivityStatus probe(bool force = false);
  PublicConnectivityStatus status() const;

 private:
  void refresh_locked(PublicConnectivityStatus& status) DANNAN_REQUIRES(_work_mutex);
  void probe_locked(PublicConnectivityStatus& status, bool force) DANNAN_REQUIRES(_work_mutex);
  void remove_owned_locked() noexcept DANNAN_REQUIRES(_work_mutex);

  const ConnectivityConfig _config;
  const identity::NodeId _node;
  const net::Endpoint _configured;
  const std::shared_ptr<Gateway> _gateway;
  const ExternalIpLookup _lookup;
  const SelfProbe _probe;
  const contract::WallClock* const _wall;
  mutable contract::IoMutex _work_mutex;
  mutable contract::Mutex _mutex;
  PublicConnectivityStatus _status DANNAN_GUARDED_BY(_mutex);
  bool _owned DANNAN_GUARDED_BY(_work_mutex) = false;
  uint16_t _owned_port DANNAN_GUARDED_BY(_work_mutex) = 0;
};

}  // namespace dannan::reach
