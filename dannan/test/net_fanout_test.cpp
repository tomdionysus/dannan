//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Channels (the plan's "Level 1": notify, stream, bulk; subscription
// node-local by topic prefix, gossiped). Two Fanouts wired to each other in
// memory, so a test decides which frames arrive: the stream's gap is
// refilled from the sender's buffer, and what the buffer no longer holds is
// reported as lost. The cluster cases are in node_test.cpp.

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <future>
#include <thread>

#include "dannan/net/fanout.h"

namespace dannan::net {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using membership::NodeRecord;

struct Io {
  asio::io_context context;
  asio::executor_work_guard<asio::io_context::executor_type> work{context.get_executor()};
  std::thread thread{[this] { context.run(); }};
  ~Io() {
    work.reset();
    context.stop();
    thread.join();
  }
};

// Two nodes in one process: each Fanout's sends and calls land on the
// other's handlers, unless the test drops them.
struct Pair {
  Io io;
  std::mutex mutex;
  NodeRecord a, b;
  std::shared_ptr<Fanout> fa, fb;
  std::function<bool(const ChannelMessage&)> drop_to_b = [](const ChannelMessage&) { return false; };

  Fanout::Hooks hooks(NodeRecord& self, NodeRecord& other, std::shared_ptr<Fanout>& peer, bool sender) {
    Fanout::Hooks out;
    out.self = [&self] { return self; };
    out.roster = [this, &self, &other] {
      std::lock_guard lock(mutex);
      return std::vector<NodeRecord>{self, other};
    };
    out.incarnation = self.id;
    out.advertise = [this, &self](const std::string& key, std::optional<Bytes> value) {
      std::lock_guard lock(mutex);
      if (value)
        self.adverts[key] = *value;
      else
        self.adverts.erase(key);
    };
    out.call = [&self, &peer](const NodeRecord&, MessageType type, Bytes payload, sched::FrameType, std::chrono::milliseconds, asio::any_io_executor executor,
                              Client::Done done) {
      Handled handled = Handled::unrecognised();
      const Message request{type, std::move(payload), {}};
      if (type == message::channel_replay) handled = peer->on_replay(self, request);
      if (type == message::channel_bulk) handled = peer->on_bulk(self, request);
      if (type == message::ping) handled = Handled::processed();
      asio::post(executor, [done, reply = handled.take_reply()] { done({reply, {}, {}}); });
    };
    out.send = [this, &self, &peer, sender](const identity::NodeId&, const Message& message, sched::FrameType, bool) {
      if (sender) {
        Reader reader(message.payload);
        ChannelMessage peek;
        peek.channel = reader.string();
        peek.topic = reader.string();
        (void)reader.u8();
        (void)reader.fixed<16>();
        peek.sequence = reader.u64();
        if (drop_to_b(peek)) return true;  // sent, and lost on the way
      }
      (void)peer->on_publish(self, message);
      return true;
    };
    out.executor = io.context.get_executor();
    return out;
  }

  Pair() {
    a.id.bytes[0] = 1;
    b.id.bytes[0] = 2;
    fa = std::make_shared<Fanout>(hooks(a, b, fb, true));
    fb = std::make_shared<Fanout>(hooks(b, a, fa, false));
  }
};

struct Inbox {
  std::mutex mutex;
  std::condition_variable arrived;
  std::vector<ChannelMessage> messages;
  ChannelHandler handler() {
    return [this](ChannelMessage message) {
      std::lock_guard lock(mutex);
      messages.push_back(std::move(message));
      arrived.notify_all();
    };
  }
  bool wait_for(size_t count) {
    std::unique_lock lock(mutex);
    return arrived.wait_for(lock, 5s, [&] { return messages.size() >= count; });
  }
};

TEST(ChannelInterest, RoundTripsAndCoarsensWhenLong) {
  EXPECT_EQ(decode_interest(encode_interest({"a/", "b/c"})), (std::vector<std::string>{"a/", "b/c"}));
  std::vector<std::string> many;
  for (int i = 0; i < 200; ++i) many.push_back("topic/" + std::to_string(i) + "/");
  EXPECT_EQ(decode_interest(encode_interest(many)), (std::vector<std::string>{""}));
  EXPECT_TRUE(decode_interest(Bytes{1, 2, 3}).empty());
  EXPECT_EQ(interest_advert("lir.seq"), "dannan.ch.lir.seq");
}

// A subscription is advertised as its prefixes, the covered ones folded
// away, and withdrawn with the last subscriber.
TEST(Channels, SubscriptionsAreAdvertised) {
  Pair pair;
  pair.fb->open("events", Delivery::notify, 0);
  Inbox inbox;
  const auto first = pair.fb->subscribe("events", "a/", pair.io.context.get_executor(), inbox.handler());
  const auto second = pair.fb->subscribe("events", "a/b/", pair.io.context.get_executor(), inbox.handler());
  EXPECT_EQ(decode_interest(*pair.b.advert("dannan.ch.events")), (std::vector<std::string>{"a/"}));
  pair.fb->unsubscribe(first);
  EXPECT_EQ(decode_interest(*pair.b.advert("dannan.ch.events")), (std::vector<std::string>{"a/b/"}));
  pair.fb->unsubscribe(second);
  EXPECT_FALSE(pair.b.advert("dannan.ch.events"));
  EXPECT_THROW(pair.fb->subscribe("absent", "", pair.io.context.get_executor(), inbox.handler()), std::invalid_argument);
  EXPECT_THROW(pair.fb->open("events", Delivery::notify, 0), std::invalid_argument);
  EXPECT_THROW(pair.fb->open("", Delivery::notify, 0), std::invalid_argument);
}

// A publish goes only to a node with a matching subscriber; nobody
// listening costs nothing.
TEST(Channels, APublishGoesOnlyWhereSomeoneListens) {
  Pair pair;
  for (auto* f : {pair.fa.get(), pair.fb.get()}) f->open("events", Delivery::notify, 0);
  Inbox inbox;
  pair.fb->subscribe("events", "a/", pair.io.context.get_executor(), inbox.handler());
  const auto sent = pair.fa->publish("events", "a/x", Bytes{7});
  EXPECT_EQ(sent.to, std::vector<identity::NodeId>{pair.b.id});
  EXPECT_TRUE(pair.fa->publish("events", "b/x", Bytes{8}).to.empty());
  ASSERT_TRUE(inbox.wait_for(1));
  EXPECT_EQ(inbox.messages[0].origin, pair.a.id);
  EXPECT_EQ(inbox.messages[0].topic, "a/x");
  EXPECT_EQ(inbox.messages[0].payload, Bytes{7});
  EXPECT_EQ(inbox.messages[0].sequence, 0u);
}

// A stream numbers each origin's messages to each destination; a gap is
// refilled from the sender's buffer, and delivery stays in order.
TEST(Channels, AStreamRefillsAGapFromTheSender) {
  Pair pair;
  for (auto* f : {pair.fa.get(), pair.fb.get()}) f->open("seq", Delivery::stream, 1 << 20);
  Inbox inbox;
  pair.fb->subscribe("seq", "", pair.io.context.get_executor(), inbox.handler());
  pair.drop_to_b = [](const ChannelMessage& m) { return m.sequence == 2 || m.sequence == 3; };
  for (uint8_t i = 1; i <= 5; ++i) pair.fa->publish("seq", "t", Bytes{i});
  ASSERT_TRUE(inbox.wait_for(5));
  for (uint8_t i = 1; i <= 5; ++i) {
    EXPECT_EQ(inbox.messages[i - 1].sequence, i);
    EXPECT_EQ(inbox.messages[i - 1].payload, Bytes{i});
    EXPECT_EQ(inbox.messages[i - 1].lost, 0u);
  }
}

// What the sender's buffer no longer holds is lost, and the first message
// after the gap says how many.
TEST(Channels, AStreamReportsWhatItCouldNotRefill) {
  Pair pair;
  pair.fa->open("seq", Delivery::stream, 3);  // bytes: holds about one message
  pair.fb->open("seq", Delivery::stream, 3);
  Inbox inbox;
  pair.fb->subscribe("seq", "", pair.io.context.get_executor(), inbox.handler());
  pair.drop_to_b = [](const ChannelMessage& m) { return m.sequence >= 2 && m.sequence <= 4; };
  for (uint8_t i = 1; i <= 5; ++i) pair.fa->publish("seq", "t", Bytes{i});
  ASSERT_TRUE(inbox.wait_for(2));
  EXPECT_EQ(inbox.messages[0].sequence, 1u);
  EXPECT_EQ(inbox.messages[1].sequence, 5u);
  EXPECT_EQ(inbox.messages[1].lost, 3u);
}

// A new incarnation of the origin restarts its sequence.
TEST(Channels, ANewIncarnationRestartsTheSequence) {
  Pair pair;
  for (auto* f : {pair.fa.get(), pair.fb.get()}) f->open("seq", Delivery::stream, 1 << 20);
  Inbox inbox;
  pair.fb->subscribe("seq", "", pair.io.context.get_executor(), inbox.handler());
  pair.fa->publish("seq", "t", Bytes{1});
  pair.fa->publish("seq", "t", Bytes{2});
  ASSERT_TRUE(inbox.wait_for(2));
  // The origin restarts: a fresh Fanout, a fresh incarnation.
  Fanout::Hooks hooks = pair.hooks(pair.a, pair.b, pair.fb, true);
  hooks.incarnation.bytes[1] = 9;
  auto restarted = std::make_shared<Fanout>(std::move(hooks));
  restarted->open("seq", Delivery::stream, 1 << 20);
  restarted->publish("seq", "t", Bytes{3});
  ASSERT_TRUE(inbox.wait_for(3));
  EXPECT_EQ(inbox.messages[2].sequence, 1u);
  EXPECT_EQ(inbox.messages[2].payload, Bytes{3});
  EXPECT_EQ(inbox.messages[2].lost, 0u);
}

// Bulk: each destination acknowledges; the publisher hears who did, and its
// own subscribers get it too.
TEST(Channels, ABulkPublishIsAcknowledged) {
  Pair pair;
  for (auto* f : {pair.fa.get(), pair.fb.get()}) f->open("bulk", Delivery::bulk, 0);
  Inbox remote, local;
  pair.fb->subscribe("bulk", "", pair.io.context.get_executor(), remote.handler());
  pair.fa->subscribe("bulk", "x", pair.io.context.get_executor(), local.handler());
  std::promise<Published> done;
  pair.fa->publish_bulk("bulk", "x/1", Bytes(1000, 3), 5s, pair.io.context.get_executor(), [&](Published p) { done.set_value(std::move(p)); });
  const auto published = done.get_future().get();
  EXPECT_EQ(published.to.size(), 2u);
  EXPECT_TRUE(published.missed.empty());
  ASSERT_TRUE(remote.wait_for(1));
  ASSERT_TRUE(local.wait_for(1));
  EXPECT_EQ(remote.messages[0].payload.size(), 1000u);
  EXPECT_THROW(pair.fa->publish("bulk", "x", {}), std::invalid_argument);
}

}  // namespace
}  // namespace dannan::net
