//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/net/fanout.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <stdexcept>

namespace dannan::net {
namespace {

namespace asio = boost::asio;
using membership::NodeRecord;

constexpr size_t max_channel_name = 64;
constexpr size_t max_topic = 4096;
constexpr size_t max_interest = 1024;
constexpr uint32_t max_replay_frames = 4096;

bool under(const std::string& topic, const std::string& prefix) { return topic.compare(0, prefix.size(), prefix) == 0; }

Bytes encode_publish(const std::string& channel, const std::string& topic, Delivery delivery, const identity::NodeId& incarnation, uint64_t sequence,
                     std::span<const uint8_t> payload) {
  Writer writer;
  writer.string(channel);
  writer.string(topic);
  writer.u8(static_cast<uint8_t>(delivery));
  writer.fixed(incarnation.bytes);
  writer.u64(sequence);
  writer.bytes(payload);
  return writer.take();
}

}  // namespace

const char* delivery_name(Delivery delivery) noexcept {
  switch (delivery) {
    case Delivery::notify:
      return "notify";
    case Delivery::stream:
      return "stream";
    case Delivery::bulk:
      return "bulk";
  }
  return "unknown";
}

std::string interest_advert(const std::string& channel) { return "dannan.ch." + channel; }

Bytes encode_interest(const std::vector<std::string>& prefixes) {
  Writer writer;
  writer.u16(static_cast<uint16_t>(std::min<size_t>(prefixes.size(), 0xffff)));
  for (const auto& prefix : prefixes) writer.string(prefix);
  if (writer.data().size() > max_interest || prefixes.size() > 0xffff) {
    // Too many to say exactly: everything, and the subscribers filter.
    Writer everything;
    everything.u16(1);
    everything.string("");
    return everything.take();
  }
  return writer.take();
}

std::vector<std::string> decode_interest(std::span<const uint8_t> advert) {
  std::vector<std::string> out;
  try {
    Reader reader(advert);
    const auto count = reader.u16();
    for (uint16_t i = 0; i < count; ++i) out.push_back(reader.string(max_topic));
    reader.finish();
  } catch (const DecodeError&) {
    // A malformed advert subscribes to nothing.
    out.clear();
  }
  return out;
}

Fanout::Fanout(Hooks hooks) : _hooks(std::move(hooks)) {}

void Fanout::open(std::string name, Delivery delivery, size_t replay_bytes) {
  if (name.empty() || name.size() > max_channel_name) throw std::invalid_argument("a channel name is 1 to 64 bytes");
  contract::Lock lock(_mutex);
  if (_channels.contains(name)) throw std::invalid_argument("channel " + name + " is already open");
  Channel channel;
  channel.delivery = delivery;
  channel.replay_bytes = replay_bytes;
  _channels.emplace(std::move(name), std::move(channel));
}

uint64_t Fanout::subscribe(const std::string& channel, std::string prefix, asio::any_io_executor executor, ChannelHandler handler) {
  if (prefix.size() > max_topic) throw std::invalid_argument("a topic prefix is at most 4096 bytes");
  uint64_t id;
  {
    contract::Lock lock(_mutex);
    if (!_channels.contains(channel)) throw std::invalid_argument("channel " + channel + " is not open");
    id = _next_subscription++;
    _subscriptions.emplace(id, Subscription{channel, std::move(prefix), std::move(executor), std::move(handler)});
  }
  readvertise(channel);
  return id;
}

void Fanout::unsubscribe(uint64_t id) {
  std::string channel;
  {
    contract::Lock lock(_mutex);
    const auto found = _subscriptions.find(id);
    if (found == _subscriptions.end()) return;
    channel = found->second.channel;
    _subscriptions.erase(found);
  }
  readvertise(channel);
}

void Fanout::readvertise(const std::string& channel) {
  std::vector<std::string> prefixes;
  {
    contract::Lock lock(_mutex);
    for (const auto& [_, subscription] : _subscriptions)
      if (subscription.channel == channel) prefixes.push_back(subscription.prefix);
  }
  std::sort(prefixes.begin(), prefixes.end());
  prefixes.erase(std::unique(prefixes.begin(), prefixes.end()), prefixes.end());
  // A prefix covered by a shorter one adds nothing.
  std::vector<std::string> minimal;
  for (const auto& prefix : prefixes)
    if (minimal.empty() || !under(prefix, minimal.back())) minimal.push_back(prefix);
  _hooks.advertise(interest_advert(channel), minimal.empty() ? std::nullopt : std::optional<Bytes>(encode_interest(minimal)));
}

std::vector<NodeRecord> Fanout::destinations(const std::string& channel, const std::string& topic) const {
  const auto key = interest_advert(channel);
  const auto self = _hooks.self().id;
  std::vector<NodeRecord> out;
  for (auto& node : _hooks.roster()) {
    if (node.id == self) continue;
    const auto advert = node.advert(key);
    if (!advert) continue;
    const auto prefixes = decode_interest(*advert);
    if (std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& prefix) { return under(topic, prefix); })) out.push_back(std::move(node));
  }
  return out;
}

bool Fanout::local_interest(const std::string& channel, const std::string& topic) const {
  return std::any_of(_subscriptions.begin(), _subscriptions.end(),
                     [&](const auto& item) { return item.second.channel == channel && under(topic, item.second.prefix); });
}

void Fanout::deliver_locally(ChannelMessage message) {
  std::vector<std::pair<asio::any_io_executor, ChannelHandler>> targets;
  {
    contract::Lock lock(_mutex);
    for (const auto& [_, subscription] : _subscriptions)
      if (subscription.channel == message.channel && under(message.topic, subscription.prefix))
        targets.emplace_back(subscription.executor, subscription.handler);
  }
  for (auto& [executor, handler] : targets) asio::post(executor, [handler, message] { handler(message); });
}

Published Fanout::publish(const std::string& channel, std::string topic, Bytes payload) {
  if (topic.size() > max_topic) throw std::invalid_argument("a topic is at most 4096 bytes");
  Delivery delivery;
  bool local;
  {
    contract::Lock lock(_mutex);
    const auto found = _channels.find(channel);
    if (found == _channels.end()) throw std::invalid_argument("channel " + channel + " is not open");
    delivery = found->second.delivery;
    if (delivery == Delivery::bulk) throw std::invalid_argument("channel " + channel + " is bulk: publish_bulk");
    local = local_interest(channel, topic);
  }
  Published out;
  const auto self = _hooks.self();
  for (const auto& peer : destinations(channel, topic)) {
    uint64_t sequence = 0;
    if (delivery == Delivery::stream) {
      // Sequenced and buffered whether or not it goes now: a gap the peer
      // sees later is refilled from here.
      contract::Lock lock(_mutex);
      auto& state = _channels.at(channel);
      auto& next = state.next[peer.id];
      if (!next) next = 1;
      sequence = next++;
      state.buffer.push_back({peer.id, sequence, topic, payload});
      state.buffered += topic.size() + payload.size();
      while (state.buffer.size() > 1 && state.buffered > state.replay_bytes) {
        state.buffered -= state.buffer.front().topic.size() + state.buffer.front().payload.size();
        state.buffer.erase(state.buffer.begin());
      }
    }
    const Message message{message::channel_publish, encode_publish(channel, topic, delivery, _hooks.incarnation, sequence, payload), {}};
    if (_hooks.send(peer.id, message, sched::FrameType::control, delivery == Delivery::notify)) {
      out.to.push_back(peer.id);
      continue;
    }
    out.missed.emplace_back(peer.id, delivery == Delivery::stream ? "no route yet: held for replay" : "no route, or the route is busy: dropped");
    // Warm a route for the next publish: a ping dials, or makes a path.
    _hooks.call(peer, message::ping, {}, sched::FrameType::control, _hooks.replay_deadline, _hooks.executor, [](CallOutcome) {});
  }
  if (local) {
    out.to.push_back(self.id);
    deliver_locally({self.id, _hooks.incarnation, channel, std::move(topic), std::move(payload), 0, 0});
  }
  return out;
}

void Fanout::publish_bulk(const std::string& channel, std::string topic, Bytes payload, std::chrono::milliseconds deadline, asio::any_io_executor executor,
                          std::function<void(Published)> done) {
  if (topic.size() > max_topic) throw std::invalid_argument("a topic is at most 4096 bytes");
  bool local;
  {
    contract::Lock lock(_mutex);
    const auto found = _channels.find(channel);
    if (found == _channels.end()) throw std::invalid_argument("channel " + channel + " is not open");
    if (found->second.delivery != Delivery::bulk) throw std::invalid_argument("channel " + channel + " is not bulk");
    local = local_interest(channel, topic);
  }
  const auto peers = destinations(channel, topic);
  struct Gathering {
    contract::Mutex mutex;
    Published published DANNAN_GUARDED_BY(mutex);
    size_t waiting DANNAN_GUARDED_BY(mutex) = 0;
    asio::any_io_executor executor;
    std::function<void(Published)> done;
  };
  auto gathering = std::make_shared<Gathering>();
  gathering->executor = std::move(executor);
  gathering->done = std::move(done);
  const auto self = _hooks.self();
  {
    contract::Lock lock(gathering->mutex);
    gathering->waiting = peers.size();
    if (local) gathering->published.to.push_back(self.id);
  }
  if (local) deliver_locally({self.id, _hooks.incarnation, channel, topic, payload, 0, 0});
  const auto finish = [gathering] {
    Published out;
    {
      contract::Lock lock(gathering->mutex);
      out = std::move(gathering->published);
    }
    asio::post(gathering->executor, [done = gathering->done, out = std::move(out)] { done(out); });
  };
  if (peers.empty()) return finish();
  Writer writer;
  writer.string(channel);
  writer.string(topic);
  writer.fixed(_hooks.incarnation.bytes);
  writer.bytes(payload);
  const auto body = writer.take();
  for (const auto& peer : peers) {
    _hooks.call(peer, message::channel_bulk, body, sched::FrameType::loader, deadline, _hooks.executor, [gathering, finish, id = peer.id](CallOutcome outcome) {
      bool last;
      {
        contract::Lock lock(gathering->mutex);
        if (outcome.reply && outcome.reply->type == message::ok)
          gathering->published.to.push_back(id);
        else
          gathering->published.missed.emplace_back(id, outcome.reply ? "refused" : outcome.failure);
        last = --gathering->waiting == 0;
      }
      if (last) finish();
    });
  }
}

std::vector<ChannelMessage> Fanout::settle_locked(const InboundKey& key, Inbound& inbound, const NodeRecord& origin) {
  std::vector<ChannelMessage> out;
  for (auto next = inbound.held.find(inbound.expected); next != inbound.held.end(); next = inbound.held.find(inbound.expected)) {
    out.push_back(std::move(next->second));
    inbound.held.erase(next);
    ++inbound.expected;
  }
  if (!inbound.held.empty() && !inbound.asking) {
    inbound.asking = true;
    // Asked outside the lock: the call is posted, never run inline.
    asio::post(_hooks.executor, [weak = weak_from_this(), origin, channel = key.second, incarnation = inbound.incarnation, from = inbound.expected] {
      if (const auto self = weak.lock()) self->ask_replay(origin, channel, incarnation, from);
    });
  }
  return out;
}

Handled Fanout::on_publish(const NodeRecord& from, const Message& message) {
  ChannelMessage received;
  Delivery delivery;
  try {
    Reader reader(message.payload);
    received.channel = reader.string(max_channel_name);
    received.topic = reader.string(max_topic);
    const auto kind = reader.u8();
    if (kind < 1 || kind > 3) throw DecodeError("bad delivery");
    delivery = static_cast<Delivery>(kind);
    received.incarnation.bytes = reader.fixed<16>();
    received.sequence = reader.u64();
    received.payload = reader.bytes();
    reader.finish();
  } catch (const DecodeError& error) {
    return Handled::failed(400, "invalid_request", error.what());
  }
  received.origin = from.id;
  if (delivery != Delivery::stream || !received.sequence) {
    deliver_locally(std::move(received));
    return Handled::processed();
  }
  std::vector<ChannelMessage> ready;
  {
    contract::Lock lock(_mutex);
    const InboundKey key{from.id, received.channel};
    auto& inbound = _inbound[key];
    if (inbound.incarnation != received.incarnation) inbound = Inbound{received.incarnation, 1, {}, false};
    if (received.sequence >= inbound.expected) inbound.held.emplace(received.sequence, std::move(received));
    ready = settle_locked(key, inbound, from);
  }
  for (auto& each : ready) deliver_locally(std::move(each));
  return Handled::processed();
}

void Fanout::ask_replay(const NodeRecord& origin, const std::string& channel, identity::NodeId incarnation, uint64_t from) {
  Writer writer;
  writer.string(channel);
  writer.fixed(incarnation.bytes);
  writer.u64(from);
  _hooks.call(origin, message::channel_replay, writer.take(), sched::FrameType::control, _hooks.replay_deadline, _hooks.executor,
              [weak = weak_from_this(), origin, channel, incarnation](CallOutcome outcome) {
                const auto self = weak.lock();
                if (!self) return;
                self->replayed(origin, channel, incarnation, std::move(outcome));
              });
}

void Fanout::replayed(const NodeRecord& origin, const std::string& channel, identity::NodeId incarnation, CallOutcome outcome) {
  std::vector<ChannelMessage> ready;
  {
    contract::Lock lock(_mutex);
    const InboundKey key{origin.id, channel};
    const auto found = _inbound.find(key);
    if (found == _inbound.end() || found->second.incarnation != incarnation) return;
    auto& inbound = found->second;
    inbound.asking = false;
    if (outcome.reply && outcome.reply->type == message::channel_replay_reply) {
      try {
        Reader reader(outcome.reply->payload);
        const bool known = reader.u8() != 0;
        const auto first = reader.u64();
        const auto count = reader.u32();
        if (count > max_replay_frames) throw DecodeError("too many replayed frames");
        for (uint32_t i = 0; i < count; ++i) {
          ChannelMessage frame{origin.id, incarnation, channel, {}, {}, reader.u64(), 0};
          frame.topic = reader.string(max_topic);
          frame.payload = reader.bytes();
          if (frame.sequence >= inbound.expected) inbound.held.emplace(frame.sequence, std::move(frame));
        }
        reader.finish();
        // What the origin no longer holds is lost; the first
        // message after the gap says how much.
        const auto resume = known ? first : (inbound.held.empty() ? inbound.expected : inbound.held.begin()->first);
        if (resume > inbound.expected) {
          const auto lost = resume - inbound.expected;
          inbound.expected = resume;
          if (const auto at = inbound.held.find(resume); at != inbound.held.end()) at->second.lost += lost;
        }
      } catch (const DecodeError&) {
      }
    }
    // A failed ask is retried by the next frame's gap.
    if (outcome.reply) ready = settle_locked(key, inbound, origin);
  }
  for (auto& each : ready) deliver_locally(std::move(each));
}

Handled Fanout::on_replay(const NodeRecord& from, const Message& request) {
  std::string channel;
  identity::NodeId incarnation;
  uint64_t start;
  try {
    Reader reader(request.payload);
    channel = reader.string(max_channel_name);
    incarnation.bytes = reader.fixed<16>();
    start = reader.u64();
    reader.finish();
  } catch (const DecodeError& error) {
    return Handled::failed(400, "invalid_request", error.what());
  }
  Writer writer;
  {
    contract::Lock lock(_mutex);
    const auto found = _channels.find(channel);
    const bool known = found != _channels.end() && incarnation == _hooks.incarnation;
    std::vector<const Channel::Sent*> frames;
    uint64_t first = start;
    if (known) {
      for (const auto& sent : found->second.buffer)
        if (sent.to == from.id && sent.sequence >= start && frames.size() < max_replay_frames) frames.push_back(&sent);
      if (!frames.empty()) {
        first = frames.front()->sequence;
      } else if (const auto next = found->second.next.find(from.id); next != found->second.next.end()) {
        first = next->second;
      }
    }
    writer.u8(known ? 1 : 0);
    writer.u64(first);
    writer.u32(static_cast<uint32_t>(frames.size()));
    for (const auto* sent : frames) {
      writer.u64(sent->sequence);
      writer.string(sent->topic);
      writer.bytes(sent->payload);
    }
  }
  return Handled::processed({message::channel_replay_reply, writer.take(), {}});
}

Handled Fanout::on_bulk(const NodeRecord& from, const Message& request) {
  ChannelMessage received;
  try {
    Reader reader(request.payload);
    received.channel = reader.string(max_channel_name);
    received.topic = reader.string(max_topic);
    received.incarnation.bytes = reader.fixed<16>();
    received.payload = reader.bytes();
    reader.finish();
  } catch (const DecodeError& error) {
    return Handled::failed(400, "invalid_request", error.what());
  }
  received.origin = from.id;
  {
    contract::Lock lock(_mutex);
    if (!local_interest(received.channel, received.topic)) return Handled::failed(404, "not_subscribed", "no subscriber here for that topic");
  }
  deliver_locally(std::move(received));
  return Handled::processed();
}

}  // namespace dannan::net
