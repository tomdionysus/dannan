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
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/context.h"
#include "dannan/identity/node_identity.h"
#include "dannan/identity/sealed_counters.h"
#include "dannan/membership/agreed_roster.h"
#include "dannan/membership/clock_offsets.h"
#include "dannan/membership/membership.h"
#include "dannan/net/client.h"
#include "dannan/net/server.h"
#include "dannan/net/transport.h"
#include "dannan/reach/connectivity.h"
#include "dannan/reach/dial_back.h"
#include "dannan/reach/inbound.h"
#include "dannan/reach/paths.h"
#include "dannan/sched/memory_limit.h"
#include "dannan/sched/work.h"
#include "dannan/status/observation.h"
#include "dannan/status/readiness.h"
#include "dannan/status/telemetry.h"
#include "dannan/status/thread_cpu.h"
#include "dannan/supervise/supervised.h"

/// @file
/// Dannan's node and its configuration.

namespace dannan {

/// Everything a Node is built with. Every field has a working default but the state path and a key
/// (or a certificate); config::node_config() makes one from the shared cluster.* settings.
struct NodeConfig {
  /// The node's state directory: the lock, the node id, the roster, telemetry, the inbound answer.
  std::filesystem::path state_path;
  /// The cluster key every node shares (version 1 of the wire).
  identity::ClusterKey key;
  /// The address to listen on.
  std::string listen_host = "0.0.0.0";
  /// What peers dial; the advertised endpoint may move with UPnP or the external address.
  std::string advertise_host;
  /// The port to listen on. Zero: any, and the bound port is advertised.
  uint16_t port = 7437;
  /// The node's failure domain: placement spreads copies across domains first. Empty: its host.
  std::string failure_domain;
  /// A name for people; ids are what nodes go by.
  std::string node_name;
  /// Seeds, not masters: exchanged with every heartbeat.
  std::vector<net::Endpoint> bootstrap;
  /// Whether the node accepts inbound connections: yes, no, or found out by dial-back.
  reach::Inbound inbound = reach::Inbound::automatic;
  /// The node's key custody (identity/key_custody.h), opened at start; it must have been made
  /// (KeyCustody::create). Nothing: no node key.
  std::optional<identity::KeySource> custody;
  /// The cluster's CA certificate (wire version 2). With certificate, the node proves itself by
  /// certificate. Checked at construction.
  std::optional<identity::Certificate> ca_certificate;
  /// This node's certificate, issued over its custody key and naming its node id.
  std::optional<identity::Certificate> certificate;
  /// With certificates, whether the node also speaks version 1 with `key`, while a cluster moves;
  /// false: by certificate alone.
  bool use_cluster_key = true;
  /// Whether this node forwards circuits for others (the relay capability).
  bool relay = false;
  /// Level 0: a forced path class (tests, the compose bed). Nothing: classified from what peers
  /// observe.
  std::optional<membership::PathClass> path_class;
  /// How long both ends of a punch keep trying.
  std::chrono::milliseconds punch_window{3000};
  /// How often a relayed pair tries to move up to a punched path.
  std::chrono::milliseconds path_upgrade_every{60000};
  /// As a relay: DATA circuits at once (CONTROL circuits are unconditional).
  size_t relay_data_circuits = 32;
  /// What recovers before the node is ready, by name (status::Readiness's planes).
  std::vector<std::string> planes;
  /// What a hosted system adds to the inbound change's journal line, after inbound_capable=, for the
  /// capability it is told: " hosts_extents=true".
  std::function<std::string(bool)> inbound_journal_fields;
  /// The hosted system's version, in telemetry and Status.
  std::string version;
  /// Agreement's slot count (agree/agreement.h), fixed for the cluster's life and the same on every
  /// node.
  uint32_t agreement_slots = agree::default_slots;
  /// Agreement's genesis members, which serve a slot no acceptor has recorded, as version 0. None:
  /// no slot can be founded, nor changed until a set recorded elsewhere serves it.
  std::vector<identity::NodeId> agreement_genesis;

  /// How often the node exchanges rosters with its peers.
  std::chrono::milliseconds heartbeat{5000};
  /// A peer not heard of for this long leaves the active roster.
  std::chrono::milliseconds dead_after{30000};
  /// A node not heard of for this long is forgotten. Never under twice dead_after.
  std::chrono::milliseconds forget_after = std::chrono::hours(24 * 30);
  /// How often telemetry is sampled; gossiped at most once a second.
  std::chrono::milliseconds telemetry_interval{10000};
  /// How long a dial may take.
  std::chrono::milliseconds connect_timeout{2500};
  /// A no-progress deadline for every CONTROL call. A node watches no call unless asked: a hosted
  /// system names these per call (net::CallWatch) or per type (net::Registration), since a long call
  /// is often meant to be long. Zero: none.
  std::chrono::milliseconds control_no_progress{0};
  /// The same for DATA calls.
  std::chrono::milliseconds data_no_progress{0};
  /// A stall notice for every CONTROL call; zero: none.
  std::chrono::milliseconds control_stall_notice{0};
  /// The same for DATA calls.
  std::chrono::milliseconds data_stall_notice{0};
  /// The node's own exchanges with a peer (membership, dial-back): a loaded peer is given this long
  /// before it counts as not having answered.
  std::chrono::milliseconds exchange_budget{30000};
  /// Tests only: refuses a dial by naming why (net::ClientOptions::dial_hook).
  std::function<std::string(const net::Endpoint&, net::Lane)> dial_hook;
  /// Tests only: refuses or stalls one call by peer and type, the node's own calls included
  /// (net::CallFault).
  std::function<net::CallFault(const std::optional<identity::NodeId>& peer, const net::Endpoint& endpoint, net::MessageType type)> call_fault;
  /// How long a relayed circuit waits for chunks that may have died with a session; none:
  /// dead_after.
  std::optional<std::chrono::milliseconds> relay_finish_wait;
  /// Tests only: the chunks a relay drops (reach::PathsConfig).
  std::function<bool(uint64_t sequence, bool forwarding, bool toward_b)> relay_drop_hook;
  /// Tests only: reach::PathsConfig's punch_target_hook.
  std::function<net::Endpoint(uint16_t local_port, const net::Endpoint& target)> punch_target_hook;
  /// The least time between two dial-back probes.
  std::chrono::milliseconds dial_back_min_interval{10000};
  /// How often inbound capability is probed again while the node is capable.
  std::chrono::milliseconds reprobe_while_capable = std::chrono::hours(1);
  /// The same while it is not.
  std::chrono::milliseconds reprobe_while_incapable = std::chrono::minutes(10);
  /// The largest frame on the wire; larger messages are fragmented.
  size_t max_frame = 256 * 1024;
  /// The server's workers and bounds.
  net::ServerLimits limits;
  /// Public connectivity: UPnP, the external address lookup, the self-probe.
  reach::ConnectivityConfig connectivity;
  /// The node's worker pool (P4); a hosted system sets its classes' caps and bounds here.
  sched::WorkerPoolConfig workers;
  /// The gate loader work runs through.
  sched::GateConfig loader_gate;
  /// The gate speculative work runs through.
  sched::GateConfig speculative_gate;
  /// The gates' own time, apart from the node's, so a hosted system's pacing tests step them while
  /// the node runs on real time (Macha's review). Null: `time` below.
  const contract::TimeSource* gate_time = nullptr;
  /// The gates' own process CPU reading. Empty: the process's real CPU.
  sched::ProcessCpu gate_process_cpu;
  /// The node's retained-memory budget.
  uint64_t memory_bytes = 768ull << 20;
  /// The headroom only control may use (law 1).
  uint64_t control_reserve_bytes = 64ull << 20;
  /// The headroom kept for foreground (law 2).
  uint64_t foreground_reserve_bytes = 192ull << 20;
  /// The headroom kept for loader (law 3).
  uint64_t loader_reserve_bytes = 64ull << 20;
  /// Inbound reassembly's own reserve.
  uint64_t reassembly_reserve_bytes = 32ull << 20;
  /// A ceiling on all of the node's memory, hosted parts included; zero for none. Over it every part
  /// shrinks by the same fraction and the node starts, warning once (fit_memory_limit()).
  uint64_t memory_limit_bytes = 0;
  /// Set by fit_memory_limit(): what the node logs when it is built.
  std::optional<std::string> memory_warning;
  /// Set by fit_memory_limit(): it has run.
  bool memory_fitted = false;
  /// Windows of the observations, written to observation/observations.jsonl under the state path,
  /// from before anything else starts; zero for none.
  std::chrono::milliseconds observation_interval{60000};
  /// The most the observation log keeps.
  uint64_t observation_max_bytes = 64ull << 20;
  /// The hosted system's own gauges, read at each observation window.
  std::function<std::map<std::string, uint64_t>()> observation_gauges;
  /// The hosted system's own telemetry fields, added to each local record.
  std::function<void(status::Telemetry&)> telemetry_fields;
  /// The order telemetry's fields are written in, the named ones first.
  std::vector<uint16_t> telemetry_layout;
  /// The UPnP gateway; null: UPnP not built.
  std::shared_ptr<reach::Gateway> gateway;
  /// How the external address is looked up; default: the built-in lookup.
  reach::ExternalIpLookup external_ip;
  /// How the node probes itself from outside; default: the built-in probe.
  reach::SelfProbe self_probe;

  /// The node's one log sink, handed to every component and, through the Context, to the hosted
  /// system.
  std::shared_ptr<status::LogSink> log = std::make_shared<status::ConsoleSink>();

  /// The transfer-buffer pool's bound (Context::buffers()): what it holds at most.
  size_t transfer_buffers_bound = 256ull * 1024 * 1024;
  /// The smallest buffer it keeps: a 256 KiB frame is kept, so fragments go round too.
  size_t transfer_buffers_floor = 128 * 1024;

  /// The steady time the node decides by; a test hands in a stepped one.
  const contract::TimeSource* time = &contract::steady_time_source();
  /// What the node's links judge liveness on (net::ClientOptions::liveness_time); none: `time`. So a
  /// test stepping `time` past dead_after/3 does not give up its links (Dagda's ask).
  const contract::TimeSource* liveness_time = nullptr;
  /// The wall time the node records by; a test hands in a stepped one.
  const contract::WallClock* wall = &contract::system_wall_clock();

  /// Runs `task` once `after` has passed on `time` (the simulation harness's schedule). Set, the node
  /// starts no loop threads: each loop's step runs from it on the io context, the next booked when
  /// one ends, and Time::after() and agreement's waits go through it too, so a world run on one
  /// thread orders all of them. Empty: the loops are threads, as always.
  std::function<void(std::chrono::milliseconds after, std::function<void()> task)> schedule;
  /// Draws the ids fresh on each start (the durability epoch, telemetry's boot id), which seed
  /// agreement's backoff among others: the simulation harness draws them from its seed so a run
  /// replays exactly. Empty: random.
  std::function<identity::NodeId()> fresh_id;
  /// How far ahead of this node's wall time a received hybrid-clock stamp may be and still be
  /// merged: five seconds, past the two-second offset bound a ready node keeps.
  std::chrono::milliseconds hlc_max_ahead = std::chrono::seconds(5);
  /// How far this node's clock may be from a peer's before that peer counts against it; past it
  /// against more than half of the agreed roster, the node issues no hybrid-clock stamp.
  std::chrono::milliseconds clock_offset_bound = std::chrono::seconds(2);
  /// How long a worse health must hold before this node's record advertises it (0.12.0): a check
  /// that flaps does not move placement. Draining, and any change back towards ready, is advertised
  /// at once.
  std::chrono::milliseconds health_hold = std::chrono::seconds(10);
  /// The network as the simulation harness gives it: set, the node builds no client, server or
  /// level 0 paths, and calls, sends and serves through what this returns, given the node's hooks.
  /// Empty: sockets, as always.
  std::function<std::shared_ptr<net::Transport>(net::TransportHooks)> transport;
};

/// Fits a node's memory under config.memory_limit_bytes: Dannan's retained total and reserves, and
/// `hosted`, the parts only the hosted system knows of (its caches), which shrink with them. A hosted
/// system with such parts calls this first, before building anything sized from them; without any,
/// the node calls it for itself. The node logs the warning.
void fit_memory_limit(NodeConfig& config, std::vector<sched::MemoryPart>& hosted);

/// Dannan's node: the control plane assembled, and the hosted system's Context.
///
/// The identity under a locked state path, the roster, public connectivity and the inbound answer,
/// telemetry, the transport's client and server, the control routes (ping, members, telemetry,
/// identity resets, dial request, dial-back, punch and relay), and three loops: membership (exchange
/// with seeds and known peers every heartbeat, forget at the horizon), telemetry (refresh, gossip
/// at most once a second, health), and connectivity (the advertised endpoint and the inbound answer
/// by dial-back). The node is reachable before it is ready.
///
/// The io context is the caller's, run on its threads; the loops have their own. Stop the node while
/// its io runs: stopping closes sessions on it. Once stopped, it may be destroyed after the io has
/// stopped. Thread-safe. Lock order: the client's route table before the roster's.
///
/// A node binary need not build one itself: supervise::run_node_program() does.
///
/// @code
/// boost::asio::io_context io;
/// dannan::NodeConfig config;
/// config.state_path = state_directory;
/// config.key = dannan::identity::load_cluster_key(key_file);
/// config.bootstrap = {{"127.0.0.1", 7437}};
/// dannan::Node node(io, config);
/// node.context().routes().add(my_type, my_registration, my_handler);  // before start
/// node.start();
/// // ... run io on its threads; at the end:
/// node.stop();
/// @endcode
class Node final : public Context {
 public:
  /// Takes the state lock and reads the node's state.
  /// @throws std::runtime_error if the path is in use or a state file is corrupt.
  /// @throws std::invalid_argument for a configuration that cannot run (a certificate without its CA
  ///   or custody, one not over this node's key, or revoked).
  Node(boost::asio::io_context& io, NodeConfig config);
  /// Stops, if not stopped.
  ~Node() override;
  /// Not copyable.
  Node(const Node&) = delete;
  /// Not copyable.
  Node& operator=(const Node&) = delete;

  /// Refuses a cluster nobody could reach, then listens and starts the loops. Hosted routes are
  /// frozen from here.
  void start();
  /// Asks the node's loops to stop and returns: what a signal handler or a stopping system calls
  /// before the work it depends on goes. stop() still joins them.
  void request_stop();
  /// Stops the loops, the server and the client; pending calls fail at once.
  void stop();
  /// Closes the route to `peer` on `lane`, as a failed link would; the next call makes another. Its
  /// sessions' calls fail at once.
  void close_route(const identity::NodeId& peer, net::Lane lane, const std::string& why);

  /// The port the node listens on (the one bound, when configured as zero).
  uint16_t port() const noexcept { return _bound_port.load(); }
  /// The node as its hosted system sees it.
  Context& context() noexcept { return *this; }
  /// The same, to read.
  const Context& context() const noexcept { return *this; }

  /// Fails every outbound call at once and admits no more, so a worker blocked in one can be joined;
  /// the rest of the node runs on until stop().
  void cancel_outbound_calls();

  /// Tests only, with NodeConfig::call_fault: fails the calls it stalled to `peer` (every peer when
  /// none) as "stall released".
  /// @return how many.
  size_t release_stalled_calls(const std::optional<identity::NodeId>& peer = std::nullopt);
  /// Tests only: how many calls are held stalled now.
  size_t stalled_calls() const;

  /// The roster beneath the Context, for a hosted system's facade.
  membership::Membership& membership() noexcept { return _members; }
  /// Telemetry's store beneath the Context.
  status::TelemetryStore& telemetry_store() noexcept { return _telemetry; }
  /// The inbound answer.
  reach::InboundResolver& inbound() noexcept { return _inbound; }
  /// The inbound answer, to read.
  const reach::InboundResolver& inbound() const noexcept { return _inbound; }
  /// The public connectivity status, when known.
  std::optional<reach::PublicConnectivityStatus> connectivity() const;
  /// The client's counts, when it runs.
  std::optional<net::ClientStats> client_stats() const;
  /// The server's queues and timings, when it runs.
  std::optional<net::WorkStats> work_stats() const;
  /// The node's retained-memory ledger.
  sched::RetainedMemoryLedger& memory() noexcept { return _memory; }
  /// The node's traffic by frame class, client and server together.
  status::TrafficTotals traffic() const;

  /// As Nodes::apply_identity_reset().
  bool apply_identity_reset(const membership::IdentityReset& reset);
  /// As Nodes::propagate_identity_reset().
  void propagate_identity_reset(const membership::IdentityReset& reset, std::chrono::milliseconds deadline);
  /// Asks for a telemetry refresh now (a readiness change, say).
  void signal_telemetry();

  /// As Context::version().
  uint32_t version() const noexcept override { return context_version; }
  /// As Context::nodes().
  Nodes& nodes() override;
  /// As Context::nodes().
  const Nodes& nodes() const override;
  /// As Context::routes().
  Routes& routes() override;
  /// As Context::channels().
  Channels& channels() override;
  /// As Context::identity().
  Identity& identity() override;
  /// As Context::identity().
  const Identity& identity() const override;
  /// As Context::budget().
  Budget& budget() override;
  /// As Context::faults().
  Faults& faults() override;
  /// As Context::time().
  Time& time() override;
  /// As Context::telemetry().
  Telemetry& telemetry() override;
  /// As Context::status().
  status::Surface& status() override { return _surface; }
  /// As Context::logger().
  status::ScopedLogger logger(std::string scope) const override { return {_config.log, std::move(scope)}; }
  /// As Context::events().
  status::NodeEvents& events() override { return _events; }
  /// As Context::readiness().
  status::Readiness& readiness() override { return _readiness; }
  /// As Context::readiness().
  const status::Readiness& readiness() const override { return _readiness; }
  /// As Context::activity().
  status::ActivityClocks& activity() override { return _activity; }
  /// As Context::observations().
  status::Observations& observations() override { return _observations; }
  /// As Context::wait_guard().
  sched::WaitGuard& wait_guard() override { return _wait_guard; }
  /// As Context::threads().
  supervise::SupervisedThreads& threads() override { return _threads; }
  /// As Context::threads().
  const supervise::SupervisedThreads& threads() const override { return _threads; }
  /// As Context::gates().
  sched::ClassGates& gates() override { return _gates; }
  /// As Context::workers().
  sched::WorkerPool& workers() override { return _workers; }
  /// As Context::buffers().
  std::shared_ptr<sched::BufferPool> buffers() override { return _buffers; }
  /// As Context::agreement().
  agree::Agreement& agreement() override { return *_agreement; }
  /// As Context::hlc().
  contract::HlcClock& hlc() override { return *_hlc; }
  /// As Context::diagnostics().
  Diagnostics diagnostics() const override;

  /// @cond internal
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

  net::Handled handle(const membership::NodeRecord& peer, sched::FrameType frame, net::Message request, net::Answer& answer);
  void observe_direct(const membership::NodeRecord& peer);
  // Sends this node's current record to `peer` in the members push; the answer is not awaited.
  void push_record_to(const membership::NodeRecord& peer);
  void merge(std::span<const uint8_t> payload);
  // Sets or clears an advert, then sends this node's record to every active peer over the
  // members exchange; `done` on `executor` with who holds it (Nodes::advertise).
  void push_advert(const std::string& key, std::optional<net::Bytes> value, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                   std::function<void(membership::Pushed)> done);
  std::shared_ptr<net::CallHandle> place_call(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type,
                                              net::Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                                              boost::asio::any_io_executor executor, net::Client::Done done, net::CallWatch watch = {});
  // A call from a loop thread, waited on within the deadline.
  net::CallOutcome call_wait(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint, net::MessageType type, net::Bytes payload,
                             std::chrono::milliseconds deadline, sched::FrameType frame = sched::FrameType::control, net::CallWatch watch = {});
  void membership_loop(std::stop_token stop);
  void telemetry_loop(std::stop_token stop);
  void connectivity_loop(std::stop_token stop);
  void roster_loop(std::stop_token stop);

  // The loops' steps: what one wake of each does, the same whether a thread or the schedule
  // (NodeConfig::schedule) runs it. A step that calls a peer goes on when the answer comes, its
  // continuations run on `on` (a loop's own context in a thread, so the disk stays the loop's,
  // Law 1), and `done` is called there once the step is over, or once `stopped` says so.
  using Step = std::function<void(boost::asio::any_io_executor on, std::function<void()> done)>;
  struct MembershipPass;
  struct ConnectivityState;
  // Forget the unseen, then exchange with each seed and known peer in turn.
  void membership_step(std::function<bool()> stopped, boost::asio::any_io_executor on, std::function<void()> done);
  void membership_next(const std::shared_ptr<MembershipPass>& pass);
  // The clock probes after a membership pass: one to each active peer at once, then judge_clock(),
  // then `done`.
  void probe_clocks(boost::asio::any_io_executor on, std::function<void()> done);
  // Out of bounds or back, from the offsets measured (item 6), told to the hybrid clock and the log.
  void judge_clock();
  // One exchange of rosters, then of revocations; `done` with why it failed, or empty.
  void exchange(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint, boost::asio::any_io_executor on,
                std::function<void(std::string)> done);
  void exchange_revocations(std::optional<membership::NodeRecord> peer, const net::Endpoint& endpoint, std::chrono::milliseconds deadline,
                            boost::asio::any_io_executor on, std::function<void()> done);
  void telemetry_step(contract::TimePoint& last_gossip);
  // Dannan's own checks, then the advert of this node's health on its record after the hold (item
  // 7). Run by the telemetry step.
  void health_step();
  void roster_step();
  // Refreshes asked for, the path class when due, the circuits' sweep, and the dial-back probe
  // when due.
  void connectivity_step(const std::shared_ptr<ConnectivityState>& state, boost::asio::any_io_executor on, std::function<void()> done);
  // A thread's way to run a step: on `context`, until it is done.
  static void run_step(boost::asio::io_context& context, const Step& step);
  // The schedule's way: each step, then the next after its interval, until the node stops.
  void schedule_loops();
  void scheduled_membership();
  void scheduled_telemetry(contract::TimePoint last_gossip);
  void scheduled_roster();
  void scheduled_connectivity(std::shared_ptr<ConnectivityState> state);
  // Runs `task` on the io context once `delay` has passed: through the schedule if there is one.
  void after(std::chrono::milliseconds delay, std::function<void()> task);
  // Writes the counters' noted advances; a failure is said and tried again.
  void flush_counters();
  void serve_refreshes();
  // Wakes the loops' wait. Takes _wait_mutex, so never call it holding a lock
  // the waiters' conditions take (_refresh_mutex).
  void wake_loops();
  void advertise(const reach::PublicConnectivityStatus& status);
  void refresh_public_connectivity(bool probe, bool force_probe, boost::asio::any_io_executor executor,
                                   std::function<void(std::optional<reach::PublicConnectivityStatus>)> done);
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
  // Beside custody, under a key derived from its seal root; none without custody.
  std::optional<identity::SealedCounters> _counters;
  // The hybrid clock's ceiling: in the counters, or a durable file without custody; then the
  // clock, which the roster loop keeps ahead of it.
  std::unique_ptr<contract::HlcCeiling> _hlc_ceiling;
  std::unique_ptr<contract::HybridClock> _hlc;
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
  // The roster as last agreed, which the partition reading counts (P2).
  // Each peer's clock offset, from the probes (item 6).
  membership::ClockOffsets _clock_offsets;
  membership::AgreedRoster _agreed;
  status::TelemetryStore _telemetry;
  status::NodeEvents _events;
  status::Surface _surface;
  status::Readiness _readiness;
  // The health this node's record advertises, and since when a worse one has waited out
  // health_hold: the telemetry step's own, which runs one at a time.
  status::Health _advertised_health = status::Health::ready;
  std::optional<contract::TimePoint> _worse_since;
  status::ActivityClocks _activity;
  status::Observations _observations;
  supervise::SupervisedThreads _threads;
  // The gates before the pool that runs through them, both after the threads.
  sched::ClassGates _gates;
  sched::WorkerPool _workers;
  sched::WaitGuard _wait_guard;
  sched::RetainedMemoryLedger _memory;
  // After the ledger it charges; detached as the node goes, so a holder's late give frees.
  const std::shared_ptr<sched::BufferPool> _buffers =
      std::make_shared<sched::BufferPool>(_config.transfer_buffers_bound, _config.transfer_buffers_floor, &_memory);
  const std::shared_ptr<net::TransportTraffic> _traffic = std::make_shared<net::TransportTraffic>();

  // Hosted types' wire rules, written before start and read only after;
  // their handlers bound and unbound at any time, the lock held shared
  // across each handler call, so unbind waits for calls in flight.
  net::RouteTable _table = net::RouteTable::with_dannan_types();
  std::shared_ptr<const net::RouteTable> _frozen;
  mutable contract::IoSharedMutex _handlers_mutex;
  std::map<net::MessageType, net::DeferringHandler> _handlers DANNAN_GUARDED_BY(_handlers_mutex);
  std::atomic<bool> _started{false};
  std::mutex _roster_mutex;
  std::condition_variable_any _roster_wake;
  std::atomic<bool> _roster_wanted{false};
  std::atomic<bool> _said_stopping{false};
  std::atomic<bool> _stopping{false};
  std::atomic<bool> _calls_cancelled{false};
  std::atomic<uint16_t> _bound_port{0};
  std::atomic<uint32_t> _peers_known{0};
  std::atomic<uint32_t> _peers_active{1};

  std::unique_ptr<net::Server> _server;
  std::unique_ptr<net::Client> _client;
  // What the node asks of the network (the harness's seam): the client and the server above,
  // or the harness's own. Declared after them, so it goes first.
  std::shared_ptr<net::Transport> _net;
  // Destroyed before the client it uses.
  std::unique_ptr<reach::Paths> _paths;
  // Channels; shared, since its completions hold it weakly.
  std::shared_ptr<net::Fanout> _fanout;
  // Agreement: its changes in flight hold its core, never the node.
  std::unique_ptr<agree::Agreement> _agreement;

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
  // The partition watchers and the reading they were last told of; under the
  // same lock, the agreed roster's own taken inside it.
  struct PartitionWatcher {
    boost::asio::any_io_executor executor;
    std::function<void(membership::Partition)> handler;
  };
  std::map<uint64_t, PartitionWatcher> _partition_watchers DANNAN_GUARDED_BY(_watch_mutex);
  std::optional<membership::Partition> _told_partition DANNAN_GUARDED_BY(_watch_mutex);
  membership::Partition partition_locked() const DANNAN_REQUIRES(_watch_mutex);
  // The advert watchers, under the same lock: what each last told of each
  // peer's value under its key, and when, so a change inside the rate waits.
  struct AdvertWatcher {
    std::string key;
    std::chrono::milliseconds every{};
    boost::asio::any_io_executor executor;
    std::function<void(AdvertChange)> handler;
    std::map<identity::NodeId, net::Bytes> told;
    std::map<identity::NodeId, contract::TimePoint> last;
  };
  std::map<uint64_t, AdvertWatcher> _advert_watchers DANNAN_GUARDED_BY(_watch_mutex);
  // Tells each advert watcher what changed in the active roster as last told,
  // where its rate allows; topology_changed, so at least every heartbeat.
  void tell_adverts_locked() DANNAN_REQUIRES(_watch_mutex);
  std::unique_ptr<reach::PublicConnectivity> _connectivity;
  // Refreshes the hosted system asked for, served by the connectivity loop,
  // since a refresh waits on the network. Closed once nothing will serve them.
  struct ConnectivityRefresh {
    bool probe = false;
    bool force_probe = false;
    boost::asio::any_io_executor executor;
    std::function<void(std::optional<reach::PublicConnectivityStatus>)> done;
  };
  contract::Mutex _refresh_mutex;
  std::deque<ConnectivityRefresh> _refreshes DANNAN_GUARDED_BY(_refresh_mutex);
  bool _refreshes_open DANNAN_GUARDED_BY(_refresh_mutex) = false;
  std::unique_ptr<status::ObservationRecorder> _observation_recorder;
  // A reporter for each io thread that takes a reading, under the pool's name.
  // Shared with posted work, which may outlive this node.
  struct IoCpu {
    explicit IoCpu(std::shared_ptr<status::LogSink> sink) : log(std::move(sink)) {}
    void tick();
    std::shared_ptr<status::LogSink> log;
    std::mutex mutex;
    std::map<std::thread::id, status::ThreadCpuReporter> reporters;
  };
  const std::shared_ptr<IoCpu> _io_cpu = std::make_shared<IoCpu>(_config.log);
  std::unique_ptr<reach::DialBackResponder> _dial_back;

  // Guards nothing: the loops' wait.
  std::mutex _wait_mutex;
  std::condition_variable_any _wake;
  std::atomic<uint64_t> _telemetry_demand{0};
  contract::Mutex _loops_mutex;
  std::vector<std::jthread> _loops DANNAN_GUARDED_BY(_loops_mutex);
  // The membership and connectivity threads' own contexts, where their steps' calls answer. They
  // live as long as the node: a late answer to a call that timed out may still be posted.
  boost::asio::io_context _membership_io;
  boost::asio::io_context _connectivity_io;
  // With a schedule: false once the node stops, so a step booked earlier does nothing after. It is
  // the first member of node.cpp's gate, which holds each running step and makes the stop wait for
  // one under way (0.10.1); the member keeps its type so the ABI does.
  std::shared_ptr<std::atomic<bool>> _scheduled;

  std::unique_ptr<NodesNoun> _nodes;
  std::unique_ptr<RoutesNoun> _routes_noun;
  std::unique_ptr<ChannelsNoun> _channels;
  std::unique_ptr<IdentityNoun> _identity_noun;
  std::unique_ptr<BudgetNoun> _budget;
  std::unique_ptr<FaultsNoun> _faults;
  std::unique_ptr<TimeNoun> _time;
  std::unique_ptr<TelemetryNoun> _telemetry_noun;
  /// @endcond
};

}  // namespace dannan
