//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// One authenticated session on Asio: calls, replies, notifications, transfer
// controls and close, over a socketpair. The inventory's transport rows
// "pending-reply limit per connection (512); a route that refuses says why",
// "an in-flight call can be cancelled, aborted or promoted, on both sides",
// "cluster traffic in and out by frame class", and the executor rule: every
// completion is posted to the caller's executor, never run inline.

#include <gtest/gtest.h>

#include <boost/asio/local/connect_pair.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <fstream>
#include <future>
#include <thread>

#include "dannan/net/session.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::net {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using sched::FrameType;

// An io context running on its own threads for the test's life.
struct Io {
  asio::io_context context;
  asio::executor_work_guard<asio::io_context::executor_type> work{context.get_executor()};
  std::vector<std::thread> threads;
  Io() {
    for (int i = 0; i < 3; ++i) threads.emplace_back([this] { context.run(); });
  }
  ~Io() {
    work.reset();
    context.stop();
    for (auto& thread : threads) thread.join();
  }
};

identity::ClusterKey test_key() {
  test::TempDir dir;
  std::ofstream(dir.path() / "k") << "session test";
  return identity::load_cluster_key(dir.path() / "k");
}

membership::NodeRecord record(const std::string& host) {
  membership::NodeRecord node;
  node.id = identity::NodeId::random();
  node.host = host;
  node.port = 7437;
  return node;
}

std::shared_ptr<const RouteTable> table() {
  auto routes = RouteTable::with_dannan_types();
  routes.add(MessageType{5}, {"put_object", Lanes::data, Frames::not_control, false});
  return std::make_shared<const RouteTable>(std::move(routes));
}

// A connected pair of sessions, handshaken on the given lane.
struct Pair {
  Io& io;
  std::shared_ptr<Session> dialler;
  std::shared_ptr<Session> acceptor;
  std::promise<std::string> dialler_closing;
  std::promise<std::string> acceptor_closing;
  std::shared_future<std::string> dialler_closed = dialler_closing.get_future().share();
  std::shared_future<std::string> acceptor_closed = acceptor_closing.get_future().share();
  std::shared_ptr<TransportTraffic> traffic = std::make_shared<TransportTraffic>();

  Pair(Io& io, SessionHandlers dialler_handlers, SessionHandlers acceptor_handlers, Lane lane = Lane::control, size_t max_frame = 65536,
       sched::RetainedMemoryLedger* memory = nullptr)
      : io(io) {
    asio::local::stream_protocol::socket a(io.context), b(io.context);
    asio::local::connect_pair(a, b);
    Session::Socket sa(io.context, Session::Socket::protocol_type(AF_UNIX, SOCK_STREAM), a.release());
    Session::Socket sb(io.context, Session::Socket::protocol_type(AF_UNIX, SOCK_STREAM), b.release());
    const auto key = test_key();
    std::promise<Established> dialled, accepted;
    async_dial_handshake(sa, key, record("10.0.0.1"), max_frame, lane, [&](std::exception_ptr error, Established done) {
      if (error)
        dialled.set_exception(error);
      else
        dialled.set_value(std::move(done));
    });
    async_accept_handshake(sb, key, record("10.0.0.2"), max_frame, "127.0.0.1", [&](std::exception_ptr error, Established done) {
      if (error)
        accepted.set_exception(error);
      else
        accepted.set_value(std::move(done));
    });
    auto d = dialled.get_future().get();
    auto c = accepted.get_future().get();
    dialler_handlers.closed = [this, inner = dialler_handlers.closed](std::string why) {
      if (inner) inner(why);
      dialler_closing.set_value(why);
    };
    acceptor_handlers.closed = [this, inner = acceptor_handlers.closed](std::string why) {
      if (inner) inner(why);
      acceptor_closing.set_value(why);
    };
    SessionOptions options;
    options.memory = memory;
    options.traffic = traffic;
    dialler = Session::create(std::move(sa), std::move(d), Role::dialler, table(), std::move(dialler_handlers), options);
    acceptor = Session::create(std::move(sb), std::move(c), Role::acceptor, table(), std::move(acceptor_handlers), options);
    dialler->start();
    acceptor->start();
  }
  // The close handlers reach into this object: wait until both have run.
  ~Pair() {
    dialler->close("test over");
    acceptor->close("test over");
    dialler_closed.wait_for(10s);
    acceptor_closed.wait_for(10s);
  }
};

// Answers every request with its payload, as `reply_type`.
SessionHandlers echo(MessageType reply_type = message::ok) {
  SessionHandlers handlers;
  handlers.request = [reply_type](Frame frame, Reply reply) {
    if (reply) reply({reply_type, frame.message.payload, {}});
  };
  return handlers;
}

// The promise is shared: a completion that arrives after the wait gave up
// must not reach into a finished frame.
CallOutcome call_and_wait(Session& session, MessageType type, Bytes payload, FrameType frame, asio::any_io_executor executor) {
  auto done = std::make_shared<std::promise<CallOutcome>>();
  auto future = done->get_future();
  const auto admitted = session.call(type, std::move(payload), frame, executor, [done](CallOutcome outcome) { done->set_value(std::move(outcome)); });
  if (!admitted.refused.empty()) return CallOutcome{std::nullopt, admitted.refused};
  if (future.wait_for(10s) != std::future_status::ready) return CallOutcome{std::nullopt, "test timed out"};
  return future.get();
}

TEST(Session, ACallIsAnsweredOnTheCallersExecutor) {
  Io io;
  Pair pair(io, echo(), echo(message::members_reply));
  auto strand = asio::make_strand(io.context);
  std::promise<bool> on_strand;
  std::promise<CallOutcome> outcome;
  pair.dialler->call(message::members, Bytes{1, 2, 3}, FrameType::control, strand, [&](CallOutcome result) {
    on_strand.set_value(strand.running_in_this_thread());
    outcome.set_value(std::move(result));
  });
  EXPECT_TRUE(on_strand.get_future().get());
  const auto result = outcome.get_future().get();
  ASSERT_TRUE(result.reply);
  EXPECT_EQ(result.reply->type, message::members_reply);
  EXPECT_EQ(result.reply->payload, Bytes({1, 2, 3}));
}

// Either end may call: the dialler numbers its requests odd, the acceptor
// even, so the two never collide.
TEST(Session, BothEndsCallWithRequestIdsThatNeverCollide) {
  Io io;
  std::mutex mutex;
  std::vector<uint64_t> seen_by_acceptor, seen_by_dialler;
  auto record_into = [&](std::vector<uint64_t>& seen) {
    SessionHandlers handlers;
    handlers.request = [&](Frame frame, Reply reply) {
      {
        std::lock_guard lock(mutex);
        seen.push_back(frame.request_id);
      }
      reply({message::ok, {}, {}});
    };
    return handlers;
  };
  Pair pair(io, record_into(seen_by_dialler), record_into(seen_by_acceptor));
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(call_and_wait(*pair.dialler, message::members, {}, FrameType::control, io.context.get_executor()).reply);
    EXPECT_TRUE(call_and_wait(*pair.acceptor, message::members, {}, FrameType::control, io.context.get_executor()).reply);
  }
  std::lock_guard lock(mutex);
  for (const auto id : seen_by_acceptor) EXPECT_EQ(id % 2, 1u);
  for (const auto id : seen_by_dialler) EXPECT_EQ(id % 2, 0u);
}

TEST(Session, ALargeMessageCrossesInFragments) {
  Io io;
  Pair pair(io, echo(), echo(), Lane::data, 4096);
  Bytes big(1 << 20);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 31);
  const auto result = call_and_wait(*pair.dialler, MessageType{5}, big, FrameType::loader, io.context.get_executor());
  ASSERT_TRUE(result.reply);
  EXPECT_EQ(result.reply->payload, big);
}

TEST(Session, ANotificationIsDispatchedWithoutAReply) {
  Io io;
  std::promise<Frame> heard;
  SessionHandlers listening;
  listening.request = [&](Frame frame, Reply reply) {
    EXPECT_FALSE(reply);
    heard.set_value(std::move(frame));
  };
  Pair pair(io, echo(), std::move(listening));
  std::promise<bool> sent;
  pair.dialler->notify({message::telemetry, Bytes{7}, {}}, FrameType::speculative, io.context.get_executor(), [&](bool ok) { sent.set_value(ok); });
  EXPECT_TRUE(sent.get_future().get());
  const auto frame = heard.get_future().get();
  EXPECT_EQ(frame.request_id, 0u);
  EXPECT_EQ(frame.message.type, message::telemetry);
}

// The budget is per session; the refusal says why, so the caller can tell
// it from a dead link.
TEST(Session, HoldsNoMoreThanItsPendingReplyBudgetAndSaysWhy) {
  Io io;
  test::Gate gate;
  test::GateOpener opener(gate);
  SessionHandlers held;
  std::mutex mutex;
  std::vector<Reply> replies;
  bool holding = true;
  // Holds every request until released, then answers at once.
  held.request = [&](Frame, Reply reply) {
    std::lock_guard lock(mutex);
    if (holding)
      replies.push_back(std::move(reply));
    else
      reply({message::members_reply, {}, {}});
  };
  Pair pair(io, echo(), std::move(held));
  std::atomic<size_t> answered{0};
  const auto received = [&] {
    std::lock_guard lock(mutex);
    return replies.size();
  };
  // In batches the writer drains, so the outbound queue (256 messages) never
  // binds first: it is the tighter limit while calls pile up unwritten.
  for (size_t issued = 0; issued < max_pending_replies;) {
    for (size_t i = 0; i < 32; ++i, ++issued) {
      const auto admitted = pair.dialler->call(message::members, {}, FrameType::control, io.context.get_executor(), [&](CallOutcome) { ++answered; });
      ASSERT_TRUE(admitted.refused.empty()) << issued << ": " << admitted.refused;
    }
    for (int spin = 0; spin < 2000 && received() < issued; ++spin) std::this_thread::sleep_for(1ms);
    ASSERT_EQ(received(), issued);
  }
  const auto refused = pair.dialler->call(message::members, {}, FrameType::control, io.context.get_executor(), [](CallOutcome) {});
  EXPECT_NE(refused.refused.find("pending reply limit"), std::string::npos);
  // Every admitted call is answered once its handler replies.
  for (int spin = 0; spin < 1000; ++spin) {
    {
      std::lock_guard lock(mutex);
      if (replies.size() == max_pending_replies) break;
    }
    std::this_thread::sleep_for(5ms);
  }
  {
    std::lock_guard lock(mutex);
    ASSERT_EQ(replies.size(), max_pending_replies);
    holding = false;
    for (auto& reply : replies) reply({message::members_reply, {}, {}});
  }
  for (int spin = 0; spin < 1000 && answered.load() < max_pending_replies; ++spin) std::this_thread::sleep_for(5ms);
  EXPECT_EQ(answered.load(), max_pending_replies);
  EXPECT_TRUE(call_and_wait(*pair.dialler, message::members, {}, FrameType::control, io.context.get_executor()).reply);
}

// A cancelled call completes at once, the peer is told, and the reply its
// handler gives later is never delivered.
TEST(Session, ACancelledCallIsToldSoAndThePeerToo) {
  Io io;
  std::promise<uint64_t> peer_heard;
  std::promise<Reply> held_reply;
  std::atomic_bool holding_one{false};
  SessionHandlers holding;
  // Holds the first request; answers the rest.
  holding.request = [&](Frame, Reply reply) {
    if (!holding_one.exchange(true))
      held_reply.set_value(std::move(reply));
    else
      reply({message::members_reply, {}, {}});
  };
  holding.cancelled = [&](uint64_t id) { peer_heard.set_value(id); };
  Pair pair(io, echo(), std::move(holding));
  std::promise<CallOutcome> outcome;
  const auto admitted = pair.dialler->call(MessageType{999}, {}, FrameType::control, io.context.get_executor(),
                                           [&](CallOutcome result) { outcome.set_value(std::move(result)); });
  auto reply = held_reply.get_future().get();
  pair.dialler->cancel(admitted.request_id);
  const auto result = outcome.get_future().get();
  EXPECT_FALSE(result.reply);
  EXPECT_NE(result.failure.find("cancelled"), std::string::npos);
  EXPECT_EQ(peer_heard.get_future().get(), admitted.request_id);
  reply({message::ok, {}, {}});
  EXPECT_TRUE(call_and_wait(*pair.dialler, message::members, {}, FrameType::control, io.context.get_executor()).reply);
}

// A handler that throws answers its request with a 500; the session goes on.
TEST(Session, AThrowingHandlerAnswersAndTheSessionLives) {
  Io io;
  SessionHandlers throwing;
  throwing.request = [](Frame, Reply) { throw std::runtime_error("handler broke"); };
  Pair pair(io, echo(), std::move(throwing));
  for (int i = 0; i < 2; ++i) {
    const auto result = call_and_wait(*pair.dialler, message::members, {}, FrameType::control, io.context.get_executor());
    ASSERT_TRUE(result.reply);
    const auto refusal = refusal_of(*result.reply);
    ASSERT_TRUE(refusal);
    EXPECT_EQ(refusal->status, 500);
    EXPECT_EQ(refusal->message, "handler broke");
  }
  EXPECT_TRUE(pair.dialler->usable());
}

TEST(Session, APromotionReachesThePeer) {
  Io io;
  std::promise<std::pair<uint64_t, FrameType>> promoted;
  std::promise<Reply> held_reply;
  SessionHandlers holding;
  holding.request = [&](Frame, Reply reply) { held_reply.set_value(std::move(reply)); };
  holding.promoted = [&](uint64_t id, FrameType frame) { promoted.set_value({id, frame}); };
  Pair pair(io, echo(), std::move(holding), Lane::data);
  std::promise<CallOutcome> outcome;
  const auto admitted = pair.dialler->call(MessageType{5}, Bytes{1}, FrameType::speculative, io.context.get_executor(),
                                           [&](CallOutcome result) { outcome.set_value(std::move(result)); });
  auto reply = held_reply.get_future().get();
  pair.dialler->promote(admitted.request_id, FrameType::foreground);
  const auto [id, frame] = promoted.get_future().get();
  EXPECT_EQ(id, admitted.request_id);
  EXPECT_EQ(frame, FrameType::foreground);
  reply({message::ok, {}, {}});
  EXPECT_TRUE(outcome.get_future().get().reply);
}

// A closing session fails every call waiting on it, and each side's owner
// hears that it closed.
TEST(Session, ClosingFailsEveryPendingCallAndTellsBothSides) {
  Io io;
  std::promise<Reply> held_reply;
  SessionHandlers holding;
  holding.request = [&](Frame, Reply reply) { held_reply.set_value(std::move(reply)); };
  Pair pair(io, echo(), std::move(holding));
  std::promise<CallOutcome> outcome;
  pair.dialler->call(message::members, {}, FrameType::control, io.context.get_executor(), [&](CallOutcome result) { outcome.set_value(std::move(result)); });
  auto reply = held_reply.get_future().get();
  pair.acceptor->close("shutting down");
  const auto result = outcome.get_future().get();
  EXPECT_FALSE(result.reply);
  EXPECT_FALSE(result.failure.empty());
  EXPECT_EQ(pair.acceptor_closed.get(), "shutting down");
  EXPECT_FALSE(pair.dialler_closed.get().empty());
  EXPECT_FALSE(pair.dialler->usable());
  // A reply after close goes nowhere, and does not crash.
  reply({message::ok, {}, {}});
}

// A call on a closed session still completes, on its executor, never inline.
TEST(Session, ACallOnAClosedSessionIsRefusedAndNeverCompletesInline) {
  Io io;
  Pair pair(io, echo(), echo());
  pair.dialler->close("done");
  const auto refused =
      pair.dialler->call(message::members, {}, FrameType::control, io.context.get_executor(), [](CallOutcome) { ADD_FAILURE() << "completed"; });
  EXPECT_FALSE(refused.refused.empty());
}

TEST(Session, CountsTrafficByClassInBothDirections) {
  Io io;
  Pair pair(io, echo(), echo(), Lane::data);
  ASSERT_TRUE(call_and_wait(*pair.dialler, MessageType{5}, Bytes(10000, 1), FrameType::loader, io.context.get_executor()).reply);
  const auto loader = static_cast<size_t>(FrameType::loader);
  // Both sessions share one count here: what went out came in.
  EXPECT_EQ(pair.traffic->out_bytes[loader].load(), pair.traffic->in_bytes[loader].load());
  EXPECT_GE(pair.traffic->out_bytes[loader].load(), 10000u + frame_overhead);
}

// Retirement drains, then closes: nothing new is admitted, what is in flight
// is answered, and once both ends have said so (protocol 23's session_retire)
// both close. A connection that lost a simultaneous dial is retired, never
// cut, so no call on it fails.
TEST(Session, RetiringAdmitsNothingNewAndClosesOnceDrained) {
  Io io;
  std::mutex mutex;
  std::vector<Reply> held;
  std::promise<void> arrived;
  SessionHandlers holding;
  holding.request = [&](Frame, Reply reply) {
    std::lock_guard lock(mutex);
    held.push_back(std::move(reply));
    arrived.set_value();
  };
  Pair pair(io, echo(), holding);
  auto done = std::make_shared<std::promise<CallOutcome>>();
  const auto admitted = pair.dialler->call(message::members, {}, FrameType::control, io.context.get_executor(),
                                           [done](CallOutcome outcome) { done->set_value(std::move(outcome)); });
  ASSERT_TRUE(admitted.refused.empty());
  arrived.get_future().wait();
  pair.dialler->retire();
  EXPECT_FALSE(pair.dialler->usable());
  const auto refused = call_and_wait(*pair.dialler, message::members, {}, FrameType::control, io.context.get_executor());
  EXPECT_FALSE(refused.reply);
  EXPECT_NE(refused.failure.find("retiring"), std::string::npos) << refused.failure;
  // Still draining: neither end has closed.
  EXPECT_EQ(pair.dialler_closed.wait_for(100ms), std::future_status::timeout);
  {
    std::lock_guard lock(mutex);
    held.front()({message::members_reply, {}, {}});
  }
  auto future = done->get_future();
  ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
  const auto outcome = future.get();
  ASSERT_TRUE(outcome.reply) << outcome.failure;
  EXPECT_EQ(outcome.reply->type, message::members_reply);
  ASSERT_EQ(pair.dialler_closed.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(pair.acceptor_closed.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(pair.dialler_closed.get(), "retired");
  EXPECT_EQ(pair.acceptor_closed.get(), "retired");
}

// The peer's retirement is honoured: this end stops admitting, drains its own
// calls, and answers with its own notice.
TEST(Session, APeersRetirementRetiresThisEndToo) {
  Io io;
  Pair pair(io, echo(), echo());
  pair.acceptor->retire();
  ASSERT_EQ(pair.dialler_closed.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(pair.acceptor_closed.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(pair.dialler_closed.get(), "retired");
  EXPECT_FALSE(pair.dialler->usable());
}

// Closing from any thread while the writer is sealing a fragment is safe:
// the queue the fragment lives in is dropped on the session's strand, not
// under the closer (a use-after-free the liveness close found).
TEST(Session, ClosingFromAnotherThreadMidTransferIsSafe) {
  Io io;
  for (int round = 0; round < 20; ++round) {
    Pair pair(io, echo(), echo(), Lane::data, 4096);
    auto done = std::make_shared<std::promise<CallOutcome>>();
    pair.dialler->call(MessageType{5}, Bytes(2 << 20, 9), FrameType::loader, io.context.get_executor(),
                       [done](CallOutcome outcome) { done->set_value(std::move(outcome)); });
    while (pair.traffic->out_bytes[static_cast<size_t>(FrameType::loader)].load() < 64 * 1024) std::this_thread::yield();
    pair.dialler->close("closed mid-transfer");
    const auto outcome = done->get_future().get();
    EXPECT_FALSE(outcome.reply);
  }
}

}  // namespace
}  // namespace dannan::net
