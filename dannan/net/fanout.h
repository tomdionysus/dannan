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

/// @file
/// Channels: named routes with a delivery contract chosen when opened, and node-local topics.

namespace dannan::net {

/// A channel's delivery contract, chosen when it is opened and never implied.
///
/// There is no exactly-once contract, and no ordering across origins.
enum class Delivery : uint8_t {
  /// CONTROL, at most once, no sequence, dropped under pressure.
  notify = 1,
  /// CONTROL, at least once: a sequence per origin and destination, gaps refilled from the
  /// sender's bounded replay buffer, and what the buffer no longer holds reported to the subscriber
  /// as lost (ChannelMessage::lost).
  stream = 2,
  /// DATA, backpressured; each destination acknowledges, and the publisher is told who did.
  bulk = 3,
  /// CONTROL, at most once and in order per origin (0.11.1; for MQTT 5.0's ordered QoS 0, Lir): a
  /// sequence per origin and destination, as stream's, but a message not newer than the last one
  /// delivered from its origin is dropped, never held or refilled. No replay buffer, nothing asked
  /// for, dropped under pressure as notify is. A node before 0.11.1 refuses it.
  ordered = 4,
};
/// "notify", "stream", "bulk" or "ordered".
const char* delivery_name(Delivery) noexcept;

/// A message as a subscriber is given it.
struct ChannelMessage {
  /// The node that published it.
  identity::NodeId origin;
  /// The origin's process (its durability epoch): a new one restarts the origin's sequence.
  identity::NodeId incarnation;
  /// The channel it was published on.
  std::string channel;
  /// Its topic, an opaque string: wildcards and sessions are the hosted system's.
  std::string topic;
  /// What was published.
  Bytes payload;
  /// stream and ordered: its sequence from its origin to this node; zero for the others.
  uint64_t sequence = 0;
  /// stream: messages before this one that the origin no longer held when this node asked for them.
  uint64_t lost = 0;
};
/// A subscriber: given each message, posted to the executor it subscribed with.
using ChannelHandler = std::function<void(ChannelMessage)>;

/// Where a publish went: the nodes that took it (this node included when it has a subscriber), and
/// those it could not reach, with why.
struct Published {
  /// Taken.
  std::vector<identity::NodeId> to;
  /// Not reached, each with why.
  std::vector<std::pair<identity::NodeId, std::string>> missed;
};

/// The advert key a channel's subscriptions are gossiped under.
std::string interest_advert(const std::string& channel);
/// Topic prefixes as the interest advert carries them (a u16 count, then the strings), at most
/// 1 KiB: coarsened to the empty prefix (everything) when longer.
Bytes encode_interest(const std::vector<std::string>& prefixes);
/// The prefixes an interest advert carries.
/// @throws DecodeError for bytes that are not one.
std::vector<std::string> decode_interest(std::span<const uint8_t> advert);

/// The channels of one node, as Context::channels() (the plan's "Level 1").
///
/// A channel is a named route with a Delivery chosen when it is opened. Subscription is by topic
/// prefix and node-local: each node gossips, in an advert per channel, the prefixes it has
/// subscribers for, and a publish goes only to the nodes whose prefixes match, and to this node's
/// own subscribers. So a topic nobody subscribes to costs nothing. Retained values are the hosted
/// system's (a notify whose last value it keeps, in a Dagda head say).
///
/// Thread-safe. Publishing waits on nothing; a subscriber's deliveries are posted to the executor
/// it gave, in order when that executor is a strand.
///
/// @code
/// auto& channels = context.channels();
/// channels.open("events", net::Delivery::stream, 1 << 20);  // a 1 MiB replay buffer
/// channels.subscribe("events", "orders/", strand, [](net::ChannelMessage m) { handle(m); });
/// channels.publish("events", "orders/42", payload);
/// @endcode
class Fanout : public std::enable_shared_from_this<Fanout> {
 public:
  /// What the node gives a Fanout.
  struct Hooks {
    /// This node's record.
    std::function<membership::NodeRecord()> self;
    /// The active roster.
    std::function<std::vector<membership::NodeRecord>()> roster;
    /// This node's incarnation (its durability epoch).
    identity::NodeId incarnation;
    /// Sets or clears an advert on this node's record.
    std::function<void(const std::string& key, std::optional<Bytes> value)> advertise;
    /// The same, and the record pushed to every active peer: `done` on `executor` with who holds it
    /// (Nodes::advertise with a deadline). Empty: a subscribe with a deadline is told nobody does.
    std::function<void(const std::string& key, std::optional<Bytes> value, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                       std::function<void(membership::Pushed)> done)>
        push;
    /// A call to a peer whose outcome is posted to `executor`.
    std::function<void(const membership::NodeRecord& peer, MessageType type, Bytes payload, sched::FrameType frame, std::chrono::milliseconds deadline,
                       boost::asio::any_io_executor executor, Client::Done done)>
        call;
    /// A notification over the route a peer has; never dials. Best effort drops rather than queues.
    std::function<bool(const identity::NodeId& peer, const Message& message, sched::FrameType frame, bool best_effort)> send;
    /// Where the fanout's own work runs.
    boost::asio::any_io_executor executor;
    /// How long a stream receiver waits for a replay it asked for.
    std::chrono::milliseconds replay_deadline{5000};
  };

  /// Held by shared_ptr: its asynchronous completions hold it weakly.
  explicit Fanout(Hooks hooks);
  /// Not copyable.
  Fanout(const Fanout&) = delete;
  /// Not copyable.
  Fanout& operator=(const Fanout&) = delete;

  /// Opens a channel on this node. Every node that publishes or subscribes opens it alike.
  /// @param name at most 64 bytes.
  /// @param delivery its contract.
  /// @param replay_bytes stream: the sender's replay buffer; 0 for the others.
  /// @throws std::invalid_argument for an empty name, one over 64 bytes, or one already open.
  void open(std::string name, Delivery delivery, size_t replay_bytes);
  /// Subscribes to every topic under `prefix` (empty: every topic).
  /// @param executor where `handler` is posted; a strand keeps the order.
  /// @return an id for unsubscribe().
  /// @throws std::invalid_argument for a channel not open.
  uint64_t subscribe(const std::string& channel, std::string prefix, boost::asio::any_io_executor executor, ChannelHandler handler);
  /// The same, and the channel's interest pushed to every active peer at once, so a publish made
  /// after `done` reaches this subscriber.
  /// @param deadline how long the push may take.
  /// @param done_executor where `done` is posted, with who holds the interest and who did not.
  uint64_t subscribe(const std::string& channel, std::string prefix, boost::asio::any_io_executor executor, ChannelHandler handler,
                     std::chrono::milliseconds deadline, boost::asio::any_io_executor done_executor, std::function<void(membership::Pushed)> done);
  /// Ends a subscription; a delivery already posted may still arrive.
  void unsubscribe(uint64_t id);
  /// Publishes on a notify, stream or ordered channel. Waits on nothing.
  /// @return where it went, and who it missed.
  /// @throws std::invalid_argument for a channel not open, or a bulk one.
  Published publish(const std::string& channel, std::string topic, Bytes payload);
  /// Publishes on a bulk channel: `done` is posted to `executor` once every destination has
  /// acknowledged or `deadline` has passed, with who did.
  /// @throws std::invalid_argument for a channel not open, or not bulk.
  void publish_bulk(const std::string& channel, std::string topic, Bytes payload, std::chrono::milliseconds deadline, boost::asio::any_io_executor executor,
                    std::function<void(Published)> done);

  /// A peer's publish: the node routes the channel message types here.
  Handled on_publish(const membership::NodeRecord& from, const Message& message);
  /// A stream receiver's request for a gap.
  Handled on_replay(const membership::NodeRecord& from, const Message& request);
  /// A peer's bulk publish.
  Handled on_bulk(const membership::NodeRecord& from, const Message& request);

 private:
  /// @cond internal
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
  // The same, posted under the lock, so two messages accepted in order are posted in order.
  void deliver_locally_locked(const ChannelMessage& message) DANNAN_REQUIRES(_mutex);
  // Who to tell, and by when, that a readvertise was pushed.
  struct Push {
    std::chrono::milliseconds deadline;
    boost::asio::any_io_executor executor;
    std::function<void(membership::Pushed)> done;
  };
  // The channel's interest, from the subscriptions now; with `push`, sent to every active
  // peer and told who holds it.
  void readvertise(const std::string& channel, std::optional<Push> push = std::nullopt) DANNAN_EXCLUDES(_mutex, _advertising);
  // Delivers what is in order for `key`, and asks for a gap. Returns what
  // to deliver, in order.
  std::vector<ChannelMessage> settle_locked(const InboundKey& key, Inbound& inbound, const membership::NodeRecord& origin) DANNAN_REQUIRES(_mutex);
  void ask_replay(const membership::NodeRecord& origin, const std::string& channel, identity::NodeId incarnation, uint64_t from);
  void replayed(const membership::NodeRecord& origin, const std::string& channel, identity::NodeId incarnation, CallOutcome outcome) DANNAN_EXCLUDES(_mutex);

  const Hooks _hooks;
  // Held across a readvertise, from reading the subscriptions to setting the advert, so two
  // racing subscribes never leave the older list advertised last. Lock order: this, then
  // _mutex; the roster's own lock is taken inside the advertise hook, after both.
  contract::Mutex _advertising;
  mutable contract::Mutex _mutex;
  std::map<std::string, Channel> _channels DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, Subscription> _subscriptions DANNAN_GUARDED_BY(_mutex);
  // stream's per origin and channel; an ordered channel's too, its `expected` the least sequence
  // still taken (nothing held, nothing asked for).
  std::map<InboundKey, Inbound> _inbound DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_subscription DANNAN_GUARDED_BY(_mutex) = 1;
  /// @endcond
};

}  // namespace dannan::net
