//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Streams inside the world (TODO/2026-11-20-stack-plan.md, item 17; Lir's step 2): a client on
// one node dials another node's listener by address with no socket; a stopped node refuses, a
// node cut off times out on the world's clock, a stall holds bytes until resumed, and stopping a
// node severs what it held. Deterministic: one thread, the world's schedule.

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/listen/connector.h"
#include "dannan/listen/listener.h"
#include "dannan/test/fixtures/sim/streams.h"

namespace dannan::test::sim {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;

void echo(const std::shared_ptr<listen::Stream>& stream) {
  auto buffer = std::make_shared<std::array<char, 4096>>();
  stream->async_read_some(asio::buffer(*buffer), [stream, buffer](boost::system::error_code error, std::size_t read) {
    if (error) return;
    asio::async_write(*stream, asio::buffer(buffer->data(), read), [stream](boost::system::error_code written, std::size_t) {
      if (!written) echo(stream);
    });
  });
}

// A listener per node, echoing, at 10.0.0.<i>:7000, and a connector on node 0.
struct Cluster {
  World world{21};
  Time time{world};
  SimNodes nodes{world, 3};
  StreamDirectory directory{nodes};
  std::vector<std::unique_ptr<listen::StreamListener>> listeners;
  std::unique_ptr<listen::StreamConnector> connector;

  Cluster() {
    for (size_t i = 0; i < nodes.size(); ++i) {
      listen::ListenerConfig config;
      config.name = "echo" + std::to_string(i);
      config.host = "127.0.0.1";
      listeners.push_back(std::make_unique<listen::StreamListener>(world.io, config, &time));
      listeners.back()->start(world.io.get_executor(), [](std::shared_ptr<listen::Stream> stream) { echo(stream); });
      directory.add(i, host(i), 7000, *listeners.back());
    }
    listen::ConnectorConfig config;
    config.name = "client";
    config.strand_per_stream = false;
    config.dial = directory.dial(0);
    connector = std::make_unique<listen::StreamConnector>(world.io.get_executor(), config, &time);
  }
  ~Cluster() {
    for (auto& listener : listeners) listener->stop();
  }

  static std::string host(size_t i) { return "10.0.0." + std::to_string(i + 1); }

  struct Dialled {
    bool told = false;
    std::shared_ptr<listen::Stream> stream;
    std::string why;
  };
  Dialled dial(size_t to, std::chrono::milliseconds deadline = 2s) {
    auto out = std::make_shared<Dialled>();
    connector->connect(world.io.get_executor(), host(to), 7000, std::nullopt, deadline,
                       [out](std::shared_ptr<listen::Stream> stream, std::string why) { *out = {true, std::move(stream), std::move(why)}; });
    world.run_until([&] { return out->told; }, 1'000'000);
    return *out;
  }

  // Sends `text` and reads as much back, on the world's schedule; empty when it did not come.
  std::string ask(const std::shared_ptr<listen::Stream>& stream, const std::string& text, uint64_t steps = 100'000) {
    auto back = std::make_shared<std::string>(text.size(), '\0');
    auto done = std::make_shared<std::optional<boost::system::error_code>>();
    stream->send(text);
    // The buffer is held by the completion: a read still waiting when this returns writes nowhere freed.
    asio::async_read(*stream, asio::buffer(*back), [done, back](boost::system::error_code error, std::size_t) { *done = error; });
    world.run_until([&] { return done->has_value(); }, steps);
    return done->has_value() && !**done ? *back : std::string();
  }
};

TEST(SimStreams, AClientDialsAnotherNodesListenerByAddress) {
  Cluster c;
  const auto dialled = c.dial(1);
  ASSERT_TRUE(dialled.stream) << dialled.why;
  EXPECT_STREQ(dialled.stream->transport(), "memory");
  EXPECT_EQ(c.ask(dialled.stream, "hello node 1"), "hello node 1");
}

TEST(SimStreams, AStoppedNodeRefusesAndItsStreamsAreSevered) {
  Cluster c;
  const auto open = c.dial(2);
  ASSERT_TRUE(open.stream) << open.why;
  EXPECT_EQ(c.ask(open.stream, "before"), "before");
  c.listeners[2]->stop();
  c.nodes.stop(2);
  EXPECT_EQ(c.ask(open.stream, "after"), "") << "a stream of a stopped node still answered";
  const auto refused = c.dial(2);
  EXPECT_FALSE(refused.stream);
  EXPECT_NE(refused.why.find("connection refused"), std::string::npos) << refused.why;
}

TEST(SimStreams, ANodeCutOffTimesOutOnTheWorldsClock) {
  Cluster c;
  c.nodes.network().partition({{0}, {1, 2}});
  const auto start = c.world.now_ms();
  const auto cut = c.dial(1, 500ms);
  EXPECT_FALSE(cut.stream);
  EXPECT_NE(cut.why.find("within the deadline"), std::string::npos) << cut.why;
  EXPECT_GE(c.world.now_ms() - start, 500u);
  c.nodes.network().heal();
  EXPECT_TRUE(c.dial(1).stream);
}

TEST(SimStreams, AStallHoldsBytesUntilResumed) {
  Cluster c;
  const auto open = c.dial(1);
  ASSERT_TRUE(open.stream) << open.why;
  ASSERT_FALSE(c.directory.links(1).empty());
  auto link = c.directory.links(1).back();
  link->stall();
  auto back = std::make_shared<std::string>(4, '\0');
  auto done = std::make_shared<bool>(false);
  open.stream->send("held");
  asio::async_read(*open.stream, asio::buffer(*back), [done, back](boost::system::error_code, std::size_t) { *done = true; });
  c.world.run_until([&] { return *done; }, 2'000);
  EXPECT_FALSE(*done) << "a stalled link delivered";
  link->resume();
  // What was sent during the stall arrives now, in order.
  c.world.run_until([&] { return *done; }, 100'000);
  EXPECT_TRUE(*done);
  EXPECT_EQ(*back, "held");
}

TEST(SimStreams, ANodeWithNothingListeningRefuses) {
  Cluster c;
  c.directory.remove(Cluster::host(1), 7000);
  const auto refused = c.dial(1);
  EXPECT_FALSE(refused.stream);
  EXPECT_NE(refused.why.find("connection refused"), std::string::npos) << refused.why;
}

}  // namespace
}  // namespace dannan::test::sim
