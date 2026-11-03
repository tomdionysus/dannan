//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/client.h"
#include "dannan/net/scheduler.h"

// Channels (the plan's "Level 1"): a named route with a delivery contract
// chosen when it is opened and never implied, and subscription by topic
// that is node-local. Each node gossips, in an advert per channel, the
// topic prefixes it has subscribers for; a publish goes only to the nodes
// whose prefixes match, and to this node's own subscribers. A topic
// nobody subscribes to costs nothing. Dannan carries topics as opaque
// strings: wildcards, sessions and retained values are the hosted
// system's (retained is a notify whose last value the hosted system keeps
// in a Dagda head).
namespace dannan::net {

// notify: CONTROL, at most once, no sequence, dropped under pressure.
// stream: CONTROL, at least once, a sequence per origin and destination,
//   gaps refilled from the sender's bounded replay buffer, and what the
//   buffer no longer holds reported to the subscriber as lost.
// bulk: DATA, backpressured; each destination acknowledges, and the
//   publisher is told who did.
// There is no exactly-once contract and no ordering across origins.
enum class Delivery : uint8_t { notify = 1, stream = 2, bulk = 3 };
const char* delivery_name(Delivery) noexcept;

struct ChannelMessage {
  identity::NodeId origin;
  // The origin's process (its durability epoch): a new one restarts the
  // origin's sequence.
  identity::NodeId incarnation;
  std::string channel;
  std::string topic;
  Bytes payload;
  // stream: this message's sequence from its origin to this node; zero for
  // the others.
  uint64_t sequence = 0;
  // stream: messages before this one that the origin no longer held when
  // this node asked for them.
  uint64_t lost = 0;
};
using ChannelHandler = std::function<void(ChannelMessage)>;

// Where a publish went: the nodes that took it (this node included when it
// has a subscriber), and those it could not reach, with why.
struct Published {
  std::vector<identity::NodeId> to;
  std::vector<std::pair<identity::NodeId, std::string>> missed;
};

// The advert key a channel's subscriptions are gossiped under.
std::string interest_advert(const std::string& channel);
// Prefixes as the advert carries them (u16 count, strings); at most 1 KiB,
// coarsened to the empty prefix (everything) when longer.
Bytes encode_interest(const std::vector<std::string>& prefixes);
std::vector<std::string> decode_interest(std::span<const uint8_t> advert);

// The channels of one node. Thread-safe. Publishing waits on nothing;
// a subscriber's deliveries are posted to the executor it gave, in order
// when that executor is a strand.
class Fanout : public std::enable_shared_from_this<Fanout> {
 public:
  struct Hooks {
    // This node's record, and the active roster.
    std::function<membership::NodeRecord()> self;
    std::function<std::vector<membership::NodeRecord>()> roster;
    // This node's incarnation (its durability epoch).
    identity::NodeId incarnation;
    // Sets or clears an advert on this node's record.
    std::function<void(const std::string& key, std::optional<Bytes> value)> advertise;
    // A call to a peer whose outcome is posted to `executor`.
    std::function<void(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                       boost::asio::any_io_executor executor, Client::Done done)>
        call;
    // A notification over the route a peer has; never dials. Best effort
    // drops rather than queues.
    std::function<bool(const identity::NodeId& peer, const Message& message, sched::FrameType frame, bool best_effort)> send;
    boost::asio::any_io_executor executor;
    std::chrono::milliseconds replay_deadline{5000};
  };

  // Held by shared_ptr: its asynchronous completions hold it weakly.
  explicit Fanout(Hooks hooks);
  Fanout(const Fanout&) = delete;
  Fanout& operator=(const Fanout&) = delete;

  // Throws std::invalid_argument for an empty name, one over 64 bytes, or
  // one already open.
  void open(std::string name, Delivery delivery, size_t replay_bytes);
  // Throws std::invalid_argument for a channel not open.
  uint64_t subscribe(const std::string& channel, std::string prefix, boost::asio::any_io_executor executor, ChannelHandler handler);
  void unsubscribe(uint64_t id);
  // notify and stream. Throws std::invalid_argument for a channel not open
  // or a bulk one.
  Published publish(const std::string& channel, std::string topic, Bytes payload);
  // bulk: `done` posted to `executor` once every destination answered or
  // `deadline` passed. Throws std::invalid_argument for a channel not open
  // or not bulk.
  void publish_bulk(const std::string& channel, std::string topic, Bytes payload, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                    std::function<void(Published)> done);

  // The node routes the channel types here.
  Handled on_publish(const membership::NodeRecord& from, const Message& message);
  Handled on_replay(const membership::NodeRecord& from, const Message& request);
  Handled on_bulk(const membership::NodeRecord& from, const Message& request);

 private:
  struct Channel {
    Delivery delivery = Delivery::notify;
    size_t replay_bytes = 0;
    // stream, as the sender: the next sequence per destination, and what
    // was sent, oldest first, within replay_bytes.
    std::map<identity::NodeId, uint64_t> next;
    struct Sent {
      identity::NodeId to;
      uint64_t sequence = 0;
      std::string topic;
      Bytes payload;
    };
    std::vector<Sent> buffer;
    size_t buffered = 0;
  };
  struct Subscription {
    std::string channel;
    std::string prefix;
    boost::asio::any_io_executor executor;
    ChannelHandler handler;
  };
  // stream, as a receiver: per origin and channel.
  struct Inbound {
    identity::NodeId incarnation;
    uint64_t expected = 1;
    std::map<uint64_t, ChannelMessage> held;
    bool asking = false;
  };
  using InboundKey = std::pair<identity::NodeId, std::string>;

  std::vector<membership::NodeRecord> destinations(const std::string& channel, const std::string& topic) const;
  bool local_interest(const std::string& channel, const std::string& topic) const DANNAN_REQUIRES(_mutex);
  void deliver_locally(ChannelMessage message) DANNAN_EXCLUDES(_mutex);
  void readvertise(const std::string& channel) DANNAN_EXCLUDES(_mutex);
  // Delivers what is in order for `key`, and asks for a gap. Returns what
  // to deliver, in order.
  std::vector<ChannelMessage> settle_locked(const InboundKey& key, Inbound& inbound, const membership::NodeRecord& origin) DANNAN_REQUIRES(_mutex);
  void ask_replay(const membership::NodeRecord& origin, const std::string& channel, identity::NodeId incarnation, uint64_t from);
  void replayed(const membership::NodeRecord& origin, const std::string& channel, identity::NodeId incarnation, CallOutcome outcome) DANNAN_EXCLUDES(_mutex);

  const Hooks _hooks;
  mutable contract::Mutex _mutex;
  std::map<std::string, Channel> _channels DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, Subscription> _subscriptions DANNAN_GUARDED_BY(_mutex);
  std::map<InboundKey, Inbound> _inbound DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_subscription DANNAN_GUARDED_BY(_mutex) = 1;
};

}  // namespace dannan::net
