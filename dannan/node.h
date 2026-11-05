//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/context.h"
#include "dannan/identity/node_identity.h"
#include "dannan/membership/membership.h"
#include "dannan/net/client.h"
#include "dannan/net/server.h"
#include "dannan/reach/connectivity.h"
#include "dannan/reach/dial_back.h"
#include "dannan/reach/inbound.h"
#include "dannan/reach/paths.h"
#include "dannan/status/readiness.h"
#include "dannan/status/telemetry.h"
#include "dannan/supervise/supervised.h"

namespace dannan {

struct NodeConfig {
  // State: the lock, the node id, the roster, telemetry, the inbound answer.
  std::filesystem::path state_path;
  identity::ClusterKey key;
  std::string listen_host = "0.0.0.0";
  // What peers dial; the advertised endpoint may move with UPnP or the
  // external address.
  std::string advertise_host;
  // Zero: any; the bound port is advertised.
  uint16_t port = 7437;
  std::string failure_domain;
  std::string node_name;
  // Seeds, not masters: exchanged with every heartbeat.
  std::vector<net::Endpoint> bootstrap;
  reach::Inbound inbound = reach::Inbound::automatic;
  // The node's key custody (identity/key_custody.h), opened at start; it
  // must have been made (KeyCustody::create). Nothing: no node key.
  std::optional<identity::KeySource> custody;
  // Certificates (wire version 2, TODO/2026-11-04-identity.md): the
  // cluster's CA and this node's certificate, issued over its custody key
  // and naming its node id; the node then proves itself by certificate.
  // With `use_cluster_key` too it also speaks version 1 with `key`, while a
  // cluster moves; without, by certificate alone. Checked at construction.
  std::optional<identity::Certificate> ca_certificate;
  std::optional<identity::Certificate> certificate;
  bool use_cluster_key = true;
  // Whether this node forwards circuits for others (the relay capability).
  bool relay = false;
  // Level 0: a forced path class (tests, the compose bed); nothing:
  // classified from what peers observe. How long both ends of a punch keep
  // trying.
  std::optional<membership::PathClass> path_class;
  std::chrono::milliseconds punch_window{3000};
  // How often a relayed pair tries to move up to a punched path.
  std::chrono::milliseconds path_upgrade_every{60000};
  // As a relay: DATA circuits at once (CONTROL circuits are unconditional).
  size_t relay_data_circuits = 32;
  // What recovers before the node is ready, by name.
  std::vector<std::string> planes;
  std::string version;

  std::chrono::milliseconds heartbeat{5000};
  std::chrono::milliseconds dead_after{30000};
  // Never under twice dead_after.
  std::chrono::milliseconds forget_after = std::chrono::hours(24 * 30);
  std::chrono::milliseconds telemetry_interval{10000};
  std::chrono::milliseconds connect_timeout{2500};
  std::chrono::milliseconds control_no_progress{30000};
  std::chrono::milliseconds data_no_progress{0};
  std::chrono::milliseconds control_stall_notice{5000};
  std::chrono::milliseconds data_stall_notice{120000};
  // Refuses a dial by naming why, for tests that fail links; see
  // net::ClientOptions::dial_hook.
  std::function<std::string(const net::Endpoint&, net::Lane)> dial_hook;
  std::chrono::milliseconds dial_back_min_interval{10000};
  std::chrono::milliseconds reprobe_while_capable = std::chrono::hours(1);
  std::chrono::milliseconds reprobe_while_incapable = std::chrono::minutes(10);
  size_t max_frame = 256 * 1024;
  net::ServerLimits limits;
  reach::ConnectivityConfig connectivity;
  // The node's retained-memory budget and its reserves.
  uint64_t memory_bytes = 768ull << 20;
  uint64_t control_reserve_bytes = 64ull << 20;
  uint64_t foreground_reserve_bytes = 192ull << 20;
  uint64_t loader_reserve_bytes = 64ull << 20;
  uint64_t reassembly_reserve_bytes = 32ull << 20;
  // The hosted system's own telemetry fields, added to each local record.
  std::function<void(status::Telemetry&)> telemetry_fields;
  std::vector<uint16_t> telemetry_layout;
  // Null: UPnP not built, the lookup and the self-probe their defaults.
  std::shared_ptr<reach::Gateway> gateway;
  reach::ExternalIpLookup external_ip;
  reach::SelfProbe self_probe;

  // The node's one log sink, handed to every component and, through the
  // Context, to the hosted system.
  std::shared_ptr<status::LogSink> log = std::make_shared<status::ConsoleSink>();

  const contract::TimeSource* time = &contract::steady_time_source();
  const contract::WallClock* wall = &contract::system_wall_clock();
};

// Dannan's node: the control plane assembled. The identity under a locked
// state path, the roster, public connectivity and the inbound answer,
// telemetry, the transport's client and server, the control routes
// (ping, members, telemetry, identity_resets, dial_request,
// dial_back, punch and relay), and three loops: membership (exchange with seeds and
// known peers every heartbeat, forget at the horizon), telemetry (refresh,
// gossip at most once a second), connectivity (the advertised endpoint and
// the inbound answer by dial-back). The node is reachable before it is
// ready. It is the hosted system's Context.
//
// The io context is the caller's, run on its threads; the loops have their
// own. Thread-safe. Lock order: the client's route table before the
// roster's (the client calls `local` with its table held).
class Node final : public Context {
 public:
  // Takes the state lock and reads the node's state; throws if the path is
  // in use or a state file is corrupt.
  Node(boost::asio::io_context& io, NodeConfig config);
  ~Node() override;
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Refuses a cluster nobody could reach, then listens and starts the
  // loops. Hosted routes are frozen from here.
  void start();
  // Stops the loops, the server and the client; pending calls fail at once.
  void stop();

  uint16_t port() const noexcept { return _bound_port.load(); }
  Context& context() noexcept { return *this; }
  const Context& context() const noexcept { return *this; }

  // Fails every outbound call at once and admits no more, so a worker
  // blocked in one can be joined; the rest of the node runs on until stop().
  void cancel_outbound_calls();

  // Beneath the Context, for the hosted system's facade.
  membership::Membership& membership() noexcept { return _members; }
  status::TelemetryStore& telemetry_store() noexcept { return _telemetry; }
  reach::InboundResolver& inbound() noexcept { return _inbound; }
  std::optional<reach::PublicConnectivityStatus> connectivity() const;
  std::optional<net::ClientStats> client_stats() const;
  std::optional<net::WorkStats> work_stats() const;
  sched::RetainedMemoryLedger& memory() noexcept { return _memory; }
  // The node's traffic by frame class, client and server together.
  status::TrafficTotals traffic() const;

  // Applies a reset everywhere it matters here; propagate also sends it to
  // every active peer and applies what they answer with.
  bool apply_identity_reset(const membership::IdentityReset& reset);
  void propagate_identity_reset(const membership::IdentityReset& reset, std::chrono::milliseconds deadline);
  // Asks for a telemetry refresh now (a readiness change, say).
  void signal_telemetry();

  // Context.
  uint32_t version() const noexcept override { return context_version; }
  Nodes& nodes() override;
  const Nodes& nodes() const override;
  Routes& routes() override;
  Channels& channels() override;
  Identity& identity() override;
  const Identity& identity() const override;
  Budget& budget() override;
  Faults& faults() override;
  Time& time() override;
  Telemetry& telemetry() override;
  status::Surface& status() override { return _surface; }
  status::ScopedLogger logger(std::string scope) const override { return {_config.log, std::move(scope)}; }
  status::NodeEvents& events() override { return _events; }
  status::Readiness& readiness() override { return _readiness; }
  status::ActivityClocks& activity() override { return _activity; }
  status::Observations& observations() override { return _observations; }
  supervise::SupervisedThreads& threads() override { return _threads; }
  Diagnostics diagnostics() const override;

 private:
  class NodesNoun;
  class RoutesNoun;
  class ChannelsNoun;
  class IdentityNoun;
  class BudgetNoun;
  class FaultsNoun;
  class TimeNoun;
  class TelemetryNoun;
  friend class RoutesNoun;

  net::Handled handle(const membership::NodeRecord& peer, sched::FrameType frame, const net::Message& request);
  void observe_direct(const membership::NodeRecord& peer);
  void merge(std::span<const uint8_t> payload);
  std::shared_ptr<net::CallHandle> place_call(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type,
                                              net::Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                                              boost::asio::any_io_executor executor, net::Client::Done done);
  // A call from a loop thread, waited on within the deadline.
  net::CallOutcome call_wait(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload,
                             std::chrono::milliseconds deadline, sched::FrameType frame = sched::FrameType::control);
  void exchange(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint);
  void membership_loop(std::stop_token stop);
  void telemetry_loop(std::stop_token stop);
  void connectivity_loop(std::stop_token stop);
  void apply_inbound(bool capable);
  // Keeps the certificate a peer proved, from a session as established.
  void note_certificate(const net::Established& session);
  std::optional<identity::Certificate> peer_certificate(const identity::NodeId& node) const;
  // Keeps the CA's revocations not yet held and closes the revoked nodes'
  // sessions; what was new.
  std::vector<identity::Revocation> apply_revocations(const std::vector<identity::Revocation>& revocations);
  // Sends this node's revocations to a peer and keeps what it answers with.
  void exchange_revocations(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint, std::chrono::milliseconds deadline);
  // The roster changed, or may have: tells the watchers what changed since
  // they were last told, and counts a topology event (with `quiet`, only
  // when something did change: a node that went silent past dead_after).
  void topology_changed(bool quiet = false);
  // Learns this node's path class and candidates; when to do it again.
  std::chrono::milliseconds classify();
  // Waits on the loops' wake until `due` or stop.
  void sleep_until(std::stop_token stop, contract::TimePoint due);

  boost::asio::io_context& _io;
  NodeConfig _config;
  identity::StateLock _lock;
  identity::NodeIdentity _identity;
  std::optional<identity::KeyCustody> _custody;
  // What this node proves itself with, and the CA's revocations it holds.
  net::Credentials _credentials;
  // Scope: the source the directory installs; read under the lock.
  mutable contract::Mutex _scope_mutex;
  std::shared_ptr<const contract::ScopeSource> _scope DANNAN_GUARDED_BY(_scope_mutex);
  std::optional<identity::RevocationStore> _revocations;
  // The certificate each peer proved in its latest certificate handshake
  // with this node, by node id.
  mutable contract::Mutex _certificates_mutex;
  std::map<identity::NodeId, identity::Certificate> _peer_certificates DANNAN_GUARDED_BY(_certificates_mutex);
  reach::InboundResolver _inbound;
  membership::Membership _members;
  status::TelemetryStore _telemetry;
  status::NodeEvents _events;
  status::Surface _surface;
  status::Readiness _readiness;
  status::ActivityClocks _activity;
  status::Observations _observations;
  supervise::SupervisedThreads _threads;
  sched::RetainedMemoryLedger _memory;
  const std::shared_ptr<net::TransportTraffic> _traffic = std::make_shared<net::TransportTraffic>();

  // Hosted types' wire rules, written before start and read only after;
  // their handlers bound and unbound at any time, the lock held shared
  // across each handler call, so unbind waits for calls in flight.
  net::RouteTable _table = net::RouteTable::with_dannan_types();
  std::shared_ptr<const net::RouteTable> _frozen;
  mutable contract::IoSharedMutex _handlers_mutex;
  std::map<net::MessageType, net::RequestHandler> _handlers DANNAN_GUARDED_BY(_handlers_mutex);
  std::atomic<bool> _started{false};
  std::atomic<bool> _stopping{false};
  std::atomic<bool> _calls_cancelled{false};
  std::atomic<uint16_t> _bound_port{0};
  std::atomic<uint32_t> _peers_known{0};
  std::atomic<uint32_t> _peers_active{1};

  std::unique_ptr<net::Server> _server;
  std::unique_ptr<net::Client> _client;
  // Destroyed before the client it uses.
  std::unique_ptr<reach::Paths> _paths;
  // Channels; shared, since its completions hold it weakly.
  std::shared_ptr<net::Fanout> _fanout;

  struct Watcher {
    boost::asio::any_io_executor executor;
    std::function<void(RosterChange)> handler;
  };
  // The roster watchers and the active roster they were last told of.
  // Lock order: taken with no other lock held; the roster is read under it.
  mutable contract::Mutex _watch_mutex;
  std::map<uint64_t, Watcher> _watchers DANNAN_GUARDED_BY(_watch_mutex);
  std::map<identity::NodeId, membership::NodeRecord> _told DANNAN_GUARDED_BY(_watch_mutex);
  uint64_t _next_watcher DANNAN_GUARDED_BY(_watch_mutex) = 1;
  std::unique_ptr<reach::PublicConnectivity> _connectivity;
  std::unique_ptr<reach::DialBackResponder> _dial_back;

  // Guards nothing: the loops' wait.
  std::mutex _wait_mutex;
  std::condition_variable_any _wake;
  std::atomic<uint64_t> _telemetry_demand{0};
  std::vector<std::jthread> _loops;

  std::unique_ptr<NodesNoun> _nodes;
  std::unique_ptr<RoutesNoun> _routes_noun;
  std::unique_ptr<ChannelsNoun> _channels;
  std::unique_ptr<IdentityNoun> _identity_noun;
  std::unique_ptr<BudgetNoun> _budget;
  std::unique_ptr<FaultsNoun> _faults;
  std::unique_ptr<TimeNoun> _time;
  std::unique_ptr<TelemetryNoun> _telemetry_noun;
};

}  // namespace dannan
