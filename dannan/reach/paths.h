//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/client.h"
#include "dannan/supervise/supervised.h"

// Level 0 (wire v1, sections 5 and 6): how two nodes meet when neither can
// dial the other. A node learns its own reachability (its path class and
// candidate addresses) from the addresses peers observe; a pair that is
// not direct is punched through both NATs by a TCP simultaneous open that
// a public node brokers, and failing that relayed through a node that
// forwards for others, which sees only ciphertext. Either way the result is
// a connected stream on which the ordinary handshake runs, proving the
// path's token inside the hello, and then an ordinary route.
namespace dannan::reach {

// The payloads, as wire v1 carries them.
struct PunchRequest {  // to a broker: punch me through to `target`
  identity::NodeId target;
  net::Lane lane = net::Lane::control;
  // Where the asker's punch port was seen from outside.
  net::Endpoint from;
};
struct PunchGo {  // from the broker to the target: `peer` is punching to you
  identity::NodeId peer;
  net::Lane lane = net::Lane::control;
  net::PathToken token{};
  net::Endpoint at;
};
struct PunchReply {  // to either: whether it goes ahead, and the other end's address
  bool accepted = false;
  std::string why;
  net::Endpoint at;
  net::PathToken token{};
};
struct RelayOpen {  // to a relay: open a circuit to `target`
  identity::NodeId target;
  net::Lane lane = net::Lane::control;
};
struct RelayOpenReply {
  uint64_t circuit = 0;  // zero: refused
  std::string why;
  net::PathToken token{};
};
struct RelayOffer {  // from the relay to the target
  uint64_t circuit = 0;
  identity::NodeId from;
  net::Lane lane = net::Lane::control;
  net::PathToken token{};
};
struct RelayOfferReply {
  bool accepted = false;
  std::string why;
};
struct RelayData {
  uint64_t circuit = 0;
  net::Bytes bytes;
};
struct RelayClose {
  uint64_t circuit = 0;
  std::string why;
};
// From the end that took chunks off the circuit, back along it: this many
// more may be sent.
struct RelayCredit {
  uint64_t circuit = 0;
  uint64_t chunks = 0;
};

// A circuit's flow control, end to end. An end sends at most this many
// chunks of at most relay_chunk_bytes beyond those the far end has written
// to its session, so what a circuit holds anywhere on the way is bounded,
// and a relay holds at most relay_window_chunks * relay_chunk_bytes for each
// direction of each circuit. Credit goes back in batches.
inline constexpr size_t relay_chunk_bytes = 64 * 1024;
inline constexpr uint64_t relay_window_chunks = 32;
inline constexpr uint64_t relay_credit_batch = 8;

net::Bytes encode(const PunchRequest&);
net::Bytes encode(const PunchGo&);
net::Bytes encode(const PunchReply&);
net::Bytes encode(const RelayOpen&);
net::Bytes encode(const RelayOpenReply&);
net::Bytes encode(const RelayOffer&);
net::Bytes encode(const RelayOfferReply&);
net::Bytes encode(const RelayData&);
net::Bytes encode(const RelayClose&);
net::Bytes encode(const RelayCredit&);
// Each throws net::DecodeError.
PunchRequest decode_punch_request(std::span<const uint8_t>);
PunchGo decode_punch_go(std::span<const uint8_t>);
PunchReply decode_punch_reply(std::span<const uint8_t>);
RelayOpen decode_relay_open(std::span<const uint8_t>);
RelayOpenReply decode_relay_open_reply(std::span<const uint8_t>);
RelayOffer decode_relay_offer(std::span<const uint8_t>);
RelayOfferReply decode_relay_offer_reply(std::span<const uint8_t>);
RelayData decode_relay_data(std::span<const uint8_t>);
RelayClose decode_relay_close(std::span<const uint8_t>);
RelayCredit decode_relay_credit(std::span<const uint8_t>);

// What a node learns of its own reachability from where peers saw it.
struct Observation {
  // The local port the probes left from.
  uint16_t local_port = 0;
  // As each probed peer saw it, in order.
  std::vector<net::Endpoint> seen;
};

// A node's path class from its observations, when it takes no inbound
// connection (wire v1, section 5): punchable when two peers saw the same
// external port for one local port (an endpoint-independent mapping), or,
// with one peer, when the port was preserved; relay_only otherwise.
membership::PathClass classify(const Observation& observation);

// Waits on the network, so never on an io thread: a probe-lane handshake
// from local `port` (zero: any) to `to`, which must authenticate as
// `expected`, and the address the ack says it came from. Throws
// std::runtime_error.
std::pair<uint16_t, net::Endpoint> observe_from(boost::asio::io_context& io, const net::Credentials& key, const membership::NodeRecord& local, uint16_t port,
                                                const net::Endpoint& to, const identity::NodeId& expected, std::chrono::milliseconds timeout);

// A TCP simultaneous open from local `port` to `to`, retried until
// `within` has passed: `done` gets the connected descriptor and its address
// family, or -1 and why not. Waits on nothing.
void simultaneous_open(boost::asio::io_context& io, uint16_t port, const net::Endpoint& to, std::chrono::milliseconds within,
                       std::function<void(int descriptor, int family, std::string why)> done);

struct PathsConfig {
  // Where the workers' faults are recorded; none, nowhere readable.
  supervise::SupervisedThreads* threads = nullptr;
  net::Credentials credentials;
  // How long a probe, a broker's or relay's call, and a handshake may take.
  std::chrono::milliseconds connect_timeout{2500};
  // How long both ends keep trying a simultaneous open.
  std::chrono::milliseconds punch_window{3000};
  // Whether this node forwards circuits for others. CONTROL circuits are
  // unconditional (a member must always be able to gossip, commit and hold
  // leases); DATA circuits are budgeted: at most this many at once, the
  // rest refused, and every byte counted per circuit in Status.
  bool relay = false;
  size_t max_data_circuits = 32;
  // A forced path class (tests and the compose bed); nothing: classified.
  std::optional<membership::PathClass> forced_class;
  // How often a relayed pair whose ends are both punchable tries a punch
  // to move up a class (wire v1, section 5).
  std::chrono::milliseconds upgrade_every{60000};
};

// Relayed bytes for one circuit, for Status.
struct CircuitStats {
  uint64_t circuit = 0;
  identity::NodeId a;
  identity::NodeId b;
  net::Lane lane = net::Lane::control;
  uint64_t bytes = 0;
};

struct PathStats {
  uint64_t punches_tried = 0;
  uint64_t punches_made = 0;
  uint64_t relays_tried = 0;
  uint64_t relays_made = 0;
  uint64_t punches_brokered = 0;
  // Relayed pairs that tried to move up to a punched path, and did.
  uint64_t upgrades_tried = 0;
  uint64_t upgrades_made = 0;
  uint64_t circuits_relayed = 0;
  // DATA circuits refused past the budget.
  uint64_t data_circuits_refused = 0;
  uint64_t relayed_bytes = 0;
  std::vector<CircuitStats> circuits;
};

// Level 0 for one node: the path maker the client asks (make_path), the
// broker's and relay's answers, the target's side of each, the circuits'
// ends, and the classification. The node routes the level 0 types here.
// Thread-safe. Paths are made on its own workers, which wait on the
// network; the handlers wait too, so they run on the server's workers.
class Paths {
 public:
  // The roster, as the node knows it.
  using Roster = std::function<std::vector<membership::NodeRecord>()>;
  // A call to a known peer that waits for its outcome within the deadline.
  using Caller = std::function<net::CallOutcome(const membership::NodeRecord& peer, net::MessageType type, net::Bytes payload, std::chrono::milliseconds)>;

  Paths(boost::asio::io_context& io, PathsConfig config, net::Client& client, std::function<membership::NodeRecord()> self, Roster roster, Caller caller);
  ~Paths();
  Paths(const Paths&) = delete;
  Paths& operator=(const Paths&) = delete;

  // The client's make_path: queued for a worker, which tries a punch
  // through each broker, then each relay, and adopts the first stream made.
  void make_path(const identity::NodeId& peer, net::Lane lane, std::function<void(std::string)> failed);

  // The handlers, by type.
  net::Handled punch_request(const membership::NodeRecord& from, const net::Message& request);
  net::Handled punch_go(const membership::NodeRecord& from, const net::Message& request);
  net::Handled relay_open(const membership::NodeRecord& from, const net::Message& request);
  net::Handled relay_offer(const membership::NodeRecord& from, const net::Message& request);
  net::Handled relay_data(const membership::NodeRecord& from, const net::Message& request);
  net::Handled relay_close(const membership::NodeRecord& from, const net::Message& request);
  net::Handled relay_credit(const membership::NodeRecord& from, const net::Message& request);

  // Probes up to two public peers from one local port and classifies this
  // node; waits on the network. Nothing when there is no public peer to ask.
  std::optional<Observation> observe();
  // Drops circuits an end of which has no route left, telling the other;
  // and, at most every upgrade interval per pair, tries to move each relayed
  // pair whose ends are both punchable up to a punch. The lower node id of a
  // pair tries, so the two ends do not punch twice. Waits on nothing.
  void sweep();
  PathStats stats() const;

  // Stops the workers and closes every circuit and pump.
  void stop();

 private:
  class Pump;
  // What asynchronous completions reach this object through: cleared by
  // stop(), under its mutex, so a late completion finds nobody.
  struct Owner;
  // One direction through a relay. Credit overtakes bytes, so one direction's
  // bytes never hold back the other's; a close goes last, so a circuit's tail
  // arrives. Held chunks are bounded by the window.
  struct Leg {
    std::deque<net::Bytes> held;
    uint64_t credit = 0;
    std::optional<std::string> close;
    bool sending = false;
    bool lost = false;
  };
  struct Circuit {
    identity::NodeId a;
    identity::NodeId b;
    net::Lane lane = net::Lane::control;
    uint64_t bytes = 0;
    // What waits to go on toward each end, one message at a time.
    Leg to_a;
    Leg to_b;
  };

  void work(std::stop_token stop);
  std::string try_punch(const membership::NodeRecord& self, const membership::NodeRecord& peer, net::Lane lane);
  std::string try_relay(const membership::NodeRecord& peer, net::Lane lane);
  // Adopts `descriptor` and waits for the handshake's verdict.
  std::string adopt_and_wait(int descriptor, int family, const identity::NodeId& peer, net::Lane lane, net::Role role, net::Purpose purpose,
                             net::PathToken token);
  // An end of a circuit at this node: its pump, registered before any byte
  // can arrive for it.
  std::shared_ptr<Pump> open_end(uint64_t circuit, const identity::NodeId& relay, const identity::NodeId& far_end, net::Lane lane, int* session_end,
                                 int* family);
  void close_end(uint64_t circuit, const std::string& why, bool tell_relay);
  // Sends a relay's next message for one direction of `circuit`, if it has one
  // and none is on its way.
  void advance(uint64_t circuit, bool toward_b);
  void advanced(uint64_t circuit, bool toward_b, bool written, bool last);
  // The session toward one end is gone: the other end is told, after what
  // it is still owed.
  void lost(uint64_t circuit, bool toward_b);
  // A control notification that must arrive: waits out a full queue, and gives
  // up only when the route is gone.
  void send_until_taken(identity::NodeId peer, net::Message message);
  // Runs `f` shortly, if this object has not stopped by then.
  void shortly(std::function<void(Paths&)> f);
  std::optional<membership::NodeRecord> known(const identity::NodeId& id) const;

  // Runs `f` on this object if it has not stopped.
  static void with(const std::shared_ptr<Owner>& owner, const std::function<void(Paths&)>& f);

  boost::asio::io_context& _io;
  const PathsConfig _config;
  const std::shared_ptr<Owner> _owner;
  net::Client& _client;
  const std::function<membership::NodeRecord()> _self;
  const Roster _roster;
  const Caller _caller;

  mutable contract::Mutex _mutex;
  std::condition_variable_any _wake;
  std::deque<std::function<void()>> _queue DANNAN_GUARDED_BY(_mutex);
  bool _stopped DANNAN_GUARDED_BY(_mutex) = false;
  // As a relay: the circuits through this node.
  std::map<uint64_t, Circuit> _circuits DANNAN_GUARDED_BY(_mutex);
  // As an end: this node's pumps, by circuit.
  std::map<uint64_t, std::shared_ptr<Pump>> _ends DANNAN_GUARDED_BY(_mutex);
  // When each relayed pair may next try to move up.
  std::map<std::pair<identity::NodeId, net::Lane>, std::chrono::steady_clock::time_point> _upgrade_after DANNAN_GUARDED_BY(_mutex);
  PathStats _stats DANNAN_GUARDED_BY(_mutex);
  std::vector<std::jthread> _workers;
};

}  // namespace dannan::reach
