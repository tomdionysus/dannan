//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The server's request scheduler, without sockets: the inventory's
// transport row "server request classes with byte-bounded queues per class,
// a fast-control allow-list, an ordered pool with per-peer FIFO, rejection
// counters, per-type and per-frame timings", and "a queued request can be
// promoted or cancelled". Laws 1 to 4: each class has its own executor, so
// no class waits behind another's held workers.

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "dannan/net/scheduler.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::net {
namespace {

using namespace std::chrono_literals;
using sched::FrameType;

constexpr MessageType have_object{2003};
constexpr MessageType get_object{2004};
constexpr MessageType put_object{2005};
constexpr MessageType put_commit{1030};
constexpr MessageType accept_commit{1031};

std::shared_ptr<const RouteTable> routes() {
  auto table = RouteTable::with_dannan_types();
  table.add(have_object, {"have_object", Lanes::control, Frames::any, false});
  table.add(get_object, {"get_object", Lanes::data, Frames::not_control, false});
  table.add(put_object, {"put_object", Lanes::data, Frames::not_control, false});
  table.add(put_commit, {"put_metadata_commit", Lanes::control, Frames::any, false, false, true});
  table.add(accept_commit, {"accept_metadata_commit", Lanes::control, Frames::any, false, false, true});
  return std::make_shared<const RouteTable>(std::move(table));
}

membership::NodeRecord peer(uint8_t n) {
  membership::NodeRecord node;
  node.id.bytes[0] = n;
  return node;
}

// Submits a request and returns a future for its answer.
std::future<Message> ask(RequestScheduler& scheduler, const membership::NodeRecord& from, uint64_t id, MessageType type, FrameType frame, Bytes payload = {}) {
  auto answer = std::make_shared<std::promise<Message>>();
  auto future = answer->get_future();
  scheduler.submit(from, Frame{id, frame, {type, std::move(payload), {}}}, Reply([answer](Message reply) { answer->set_value(std::move(reply)); }));
  return future;
}

bool answered_within(std::future<Message>& future, std::chrono::milliseconds bound) { return future.wait_for(bound) == std::future_status::ready; }

// Every class keeps answering while another class's workers are all held.
TEST(Scheduler, HeldWorkersOfOneClassNeverDelayAnother) {
  struct Case {
    const char* name;
    MessageType held;
    FrameType frame;
    size_t count;
    size_t entries;
    std::vector<std::pair<MessageType, FrameType>> prompt;
  };
  const std::vector<Case> cases{
      {"every DATA worker on speculative puts",
       put_object,
       FrameType::speculative,
       8,
       6,
       {{message::ping, FrameType::control}, {message::members, FrameType::control}}},
      {"both control workers on slow handlers",
       have_object,
       FrameType::control,
       2,
       2,
       {{message::ping, FrameType::control}, {message::members, FrameType::control}}},
      {"loader puts on the DATA workers", put_object, FrameType::loader, 8, 6, {{get_object, FrameType::foreground}}},
  };
  for (const auto& c : cases) {
    test::Gate gate;
    test::GateOpener opener(gate);
    RequestScheduler scheduler(routes(), ServerLimits{}, [&](const membership::NodeRecord&, FrameType frame, const Message& request) {
      if (request.type == c.held && frame == c.frame) gate.enter_and_wait();
      return Handled::processed({message::ok, {}, {}});
    });
    std::vector<std::future<Message>> held;
    for (size_t i = 0; i < c.count; ++i) held.push_back(ask(scheduler, peer(1), 2 * i + 1, c.held, c.frame));
    ASSERT_TRUE(gate.wait_for_entries(c.entries)) << c.name;
    for (const auto& [type, frame] : c.prompt) {
      auto answer = ask(scheduler, peer(2), 1001, type, frame);
      EXPECT_TRUE(answered_within(answer, 1s)) << c.name << ": " << scheduler.routes().name(type);
    }
    gate.open();
    for (auto& future : held) EXPECT_TRUE(answered_within(future, 2s)) << c.name;
  }
}

// Ordered work: one job per peer at a time, in arrival order, while another
// peer's job may run beside it.
TEST(Scheduler, OrderedWorkKeepsEachPeersOrderAndRunsPeersSideBySide) {
  test::Gate first;
  test::GateOpener opener(first);
  std::mutex mutex;
  std::vector<uint8_t> first_peer;
  std::atomic_bool second_peer_ran{false};
  std::atomic_bool first_peer_second_ran{false};
  ServerLimits limits;
  limits.ordered_workers = 2;
  RequestScheduler scheduler(routes(), limits, [&](const membership::NodeRecord& from, FrameType, const Message& request) {
    if (from.id.bytes[0] == 1) {
      {
        std::lock_guard lock(mutex);
        first_peer.push_back(request.payload.front());
      }
      if (request.payload.front() == 1) first.enter_and_wait();
      if (request.payload.front() == 2) first_peer_second_ran = true;
    } else {
      second_peer_ran = true;
    }
    return Handled::processed({message::ok, Bytes{1}, {}});
  });
  auto a1 = ask(scheduler, peer(1), 1, put_commit, FrameType::control, Bytes{1});
  ASSERT_TRUE(first.wait_for_entries(1));
  auto a2 = ask(scheduler, peer(1), 3, put_commit, FrameType::control, Bytes{2});
  auto b1 = ask(scheduler, peer(2), 1, put_commit, FrameType::control, Bytes{3});
  EXPECT_TRUE(answered_within(b1, 2s));
  EXPECT_TRUE(second_peer_ran.load());
  EXPECT_FALSE(first_peer_second_ran.load());
  first.open();
  EXPECT_TRUE(answered_within(a1, 2s));
  EXPECT_TRUE(answered_within(a2, 2s));
  std::lock_guard lock(mutex);
  EXPECT_EQ(first_peer, (std::vector<uint8_t>{1, 2}));
}

// A commit's control-class mutation is taken before queued background ones,
// and background work cannot fill the queue they share.
TEST(Scheduler, ControlClassOrderedWorkGoesFirstAndKeepsAQuarterOfTheQueue) {
  test::Gate first;
  test::GateOpener opener(first);
  std::mutex mutex;
  std::vector<uint8_t> order;
  ServerLimits limits;
  limits.ordered_workers = 1;
  limits.ordered_pending_jobs = 4;
  limits.ordered_pending_bytes = 64;
  RequestScheduler scheduler(routes(), limits, [&](const membership::NodeRecord&, FrameType, const Message& request) {
    const auto tag = request.payload.front();
    if (tag == 1) first.enter_and_wait();
    std::lock_guard lock(mutex);
    order.push_back(tag);
    return Handled::processed({message::ok, Bytes{1}, {}});
  });
  auto running = ask(scheduler, peer(1), 1, put_commit, FrameType::speculative, Bytes{1});
  ASSERT_TRUE(first.wait_for_entries(1));
  auto b2 = ask(scheduler, peer(2), 1, put_commit, FrameType::speculative, Bytes{2});
  auto b3 = ask(scheduler, peer(3), 1, put_commit, FrameType::speculative, Bytes{3});
  auto b4 = ask(scheduler, peer(4), 1, put_commit, FrameType::speculative, Bytes{4});
  // Three of four slots are background's: a fourth queued is refused.
  auto refused = ask(scheduler, peer(5), 1, put_commit, FrameType::speculative, Bytes{6});
  ASSERT_TRUE(answered_within(refused, 2s));
  EXPECT_EQ(refused.get().type, message::error);
  auto commit = ask(scheduler, peer(6), 1, accept_commit, FrameType::control, Bytes{5});
  first.open();
  for (auto* future : {&running, &b2, &b3, &b4, &commit}) EXPECT_TRUE(answered_within(*future, 2s));
  std::lock_guard lock(mutex);
  EXPECT_EQ(order, (std::vector<uint8_t>{1, 5, 2, 3, 4}));
  EXPECT_EQ(scheduler.stats().ordered_rejected, 1u);
}

// Every class's queue is bounded in bytes; a request over it is answered at
// once with a refusal to try later, never queued, and counted.
TEST(Scheduler, AQueueOverItsByteBoundRefusesAndCounts) {
  test::Gate gate;
  test::GateOpener opener(gate);
  ServerLimits limits;
  limits.fast_control_pending_bytes = 1;
  limits.control_pending_bytes = 1;
  limits.data_pending_bytes = 1;
  limits.ordered_pending_bytes = 8;
  std::atomic<int> calls{0};
  RequestScheduler scheduler(routes(), limits, [&](const membership::NodeRecord&, FrameType, const Message&) {
    ++calls;
    return Handled::processed({message::ok, {}, {}});
  });
  auto fast = ask(scheduler, peer(1), 1, message::ping, FrameType::control, Bytes(2, 0));
  auto control = ask(scheduler, peer(1), 3, have_object, FrameType::control, Bytes(2, 0));
  auto data = ask(scheduler, peer(1), 5, get_object, FrameType::foreground, Bytes(2, 0));
  auto ordered = ask(scheduler, peer(1), 7, accept_commit, FrameType::control, Bytes(9, 0));
  for (auto* future : {&fast, &control, &data, &ordered}) {
    ASSERT_TRUE(answered_within(*future, 2s));
    const auto refusal = refusal_of(future->get());
    ASSERT_TRUE(refusal);
    EXPECT_TRUE(try_later(*refusal));
  }
  EXPECT_EQ(calls.load(), 0);
  const auto stats = scheduler.stats();
  EXPECT_EQ(stats.rejected, 3u);
  EXPECT_EQ(stats.ordered_rejected, 1u);
  // Within the bound, each is served.
  EXPECT_TRUE(answered_within(*std::make_unique<std::future<Message>>(ask(scheduler, peer(1), 9, message::ping, FrameType::control)), 2s));
}

TEST(Scheduler, RecordsTimingsPerTypeAndPerFrameClass) {
  RequestScheduler scheduler(routes(), ServerLimits{},
                             [](const membership::NodeRecord&, FrameType, const Message&) { return Handled::processed({message::ok, {}, {}}); });
  auto a = ask(scheduler, peer(1), 1, message::ping, FrameType::control);
  auto b = ask(scheduler, peer(1), 3, get_object, FrameType::foreground);
  ASSERT_TRUE(answered_within(a, 2s));
  ASSERT_TRUE(answered_within(b, 2s));
  for (int spin = 0; spin < 200 && scheduler.stats().message_timings.size() < 2; ++spin) std::this_thread::sleep_for(5ms);
  const auto stats = scheduler.stats();
  EXPECT_EQ(stats.message_timings.at(message::ping).requests, 1u);
  EXPECT_EQ(stats.message_timings.at(get_object).requests, 1u);
  EXPECT_EQ(stats.frame_timings.at(FrameType::control).requests, 1u);
  EXPECT_EQ(stats.frame_timings.at(FrameType::foreground).requests, 1u);
}

// A queued read-ahead waits behind held lower-class work until promoted to
// foreground, then runs on a worker kept for it, as foreground.
TEST(Scheduler, APromotedQueuedRequestRunsAtItsNewClass) {
  test::Gate loaders;
  test::GateOpener opener(loaders);
  std::promise<FrameType> ran_as;
  RequestScheduler scheduler(routes(), ServerLimits{}, [&](const membership::NodeRecord&, FrameType frame, const Message& request) {
    if (request.type == put_object) loaders.enter_and_wait();
    if (request.type == get_object) ran_as.set_value(frame);
    return Handled::processed({message::ok, {}, {}});
  });
  std::vector<std::future<Message>> held;
  for (uint64_t i = 0; i < 8; ++i) held.push_back(ask(scheduler, peer(1), 2 * i + 1, put_object, FrameType::loader, Bytes{1}));
  ASSERT_TRUE(loaders.wait_for_entries(6));
  auto read_ahead = ask(scheduler, peer(1), 101, get_object, FrameType::read_ahead, Bytes{1});
  EXPECT_FALSE(answered_within(read_ahead, 50ms));
  scheduler.promote(peer(1).id, 101, FrameType::foreground);
  EXPECT_TRUE(answered_within(read_ahead, 2s));
  EXPECT_EQ(ran_as.get_future().get(), FrameType::foreground);
  loaders.open();
}

TEST(Scheduler, ACancelledQueuedRequestNeverRuns) {
  test::Gate gate;
  test::GateOpener opener(gate);
  std::atomic<int> ran{0};
  ServerLimits limits;
  limits.ordered_workers = 1;
  RequestScheduler scheduler(routes(), limits, [&](const membership::NodeRecord&, FrameType, const Message& request) {
    ++ran;
    if (request.payload.front() == 1) gate.enter_and_wait();
    return Handled::processed({message::ok, Bytes{1}, {}});
  });
  auto running = ask(scheduler, peer(1), 1, put_commit, FrameType::control, Bytes{1});
  ASSERT_TRUE(gate.wait_for_entries(1));
  auto queued = ask(scheduler, peer(1), 3, put_commit, FrameType::control, Bytes{2});
  scheduler.cancel(peer(1).id, 3);
  EXPECT_EQ(scheduler.stats().ordered_pending_jobs, 0u);
  gate.open();
  EXPECT_TRUE(answered_within(running, 2s));
  EXPECT_EQ(ran.load(), 1);
}

// A handler that throws answers 500; the worker goes on.
TEST(Scheduler, AThrowingHandlerAnswersAndTheWorkerGoesOn) {
  RequestScheduler scheduler(routes(), ServerLimits{}, [](const membership::NodeRecord&, FrameType, const Message& request) -> Handled {
    if (request.payload.empty()) throw std::runtime_error("broken");
    return Handled::processed({message::ok, {}, {}});
  });
  auto broken = ask(scheduler, peer(1), 1, have_object, FrameType::control);
  ASSERT_TRUE(answered_within(broken, 2s));
  EXPECT_EQ(refusal_of(broken.get())->status, 500);
  auto fine = ask(scheduler, peer(1), 3, have_object, FrameType::control, Bytes{1});
  ASSERT_TRUE(answered_within(fine, 2s));
  EXPECT_EQ(fine.get().type, message::ok);
}

// A request's bytes stay charged until its handler is done with it.
TEST(Scheduler, ARequestIsChargedUntilItsHandlerReturns) {
  sched::RetainedMemoryLedger memory(1 << 20, 64 << 10, 256 << 10, 64 << 10, 64 << 10);
  test::Gate gate;
  test::GateOpener opener(gate);
  RequestScheduler scheduler(
      routes(), ServerLimits{},
      [&](const membership::NodeRecord&, FrameType, const Message&) {
        gate.enter_and_wait();
        return Handled::processed({message::ok, {}, {}});
      },
      &memory);
  auto answer = ask(scheduler, peer(1), 1, put_object, FrameType::loader, Bytes(64 << 10, 1));
  ASSERT_TRUE(gate.wait_for_entries(1));
  EXPECT_GE(memory.stats().used_bytes, 64u << 10);
  gate.open();
  ASSERT_TRUE(answered_within(answer, 2s));
  for (int spin = 0; spin < 200 && memory.stats().used_bytes; ++spin) std::this_thread::sleep_for(5ms);
  EXPECT_EQ(memory.stats().used_bytes, 0u);
}

// Stopping finishes the jobs that are running and drops the rest unanswered:
// their sessions are closing anyway.
TEST(Scheduler, StoppingFinishesRunningWorkAndDropsTheQueue) {
  test::Gate gate;
  test::GateOpener opener(gate);
  std::atomic<int> ran{0};
  ServerLimits limits;
  limits.ordered_workers = 1;
  auto scheduler = std::make_unique<RequestScheduler>(routes(), limits, [&](const membership::NodeRecord&, FrameType, const Message&) {
    ++ran;
    gate.enter_and_wait();
    return Handled::processed({message::ok, Bytes{1}, {}});
  });
  auto running = ask(*scheduler, peer(1), 1, put_commit, FrameType::control, Bytes{1});
  ASSERT_TRUE(gate.wait_for_entries(1));
  auto dropped = ask(*scheduler, peer(1), 3, put_commit, FrameType::control, Bytes{2});
  std::thread stopper([&] { scheduler->stop(); });
  // The queue is dropped before the running job is let go.
  while (!scheduler->stopping()) std::this_thread::yield();
  gate.open();
  stopper.join();
  EXPECT_TRUE(answered_within(running, 2s));
  EXPECT_EQ(ran.load(), 1);
  const auto stats = scheduler->stats();
  EXPECT_EQ(stats.ordered_pending_jobs, 0u);
  EXPECT_EQ(stats.ordered_active_jobs, 0u);
}

// A handler says what it did with a message: processed it (with its reply),
// failed (with its refusal), or does not recognise it, which Dannan answers
// itself with 501. Each is counted per type, so traffic a node does not host
// shows in its stats without every handler building the refusal.
TEST(Scheduler, AHandlerSaysWhetherItProcessedFailedOrDidNotRecognise) {
  constexpr MessageType foreign{1999};
  RequestScheduler scheduler(routes(), ServerLimits{}, [](const membership::NodeRecord&, FrameType, const Message& request) {
    if (request.type == foreign) return Handled::unrecognised();
    if (request.payload == Bytes{2}) return Handled::failed(409, "conflict", "already there");
    return Handled::processed({message::ok, Bytes{1}, {}});
  });
  auto done = ask(scheduler, peer(1), 1, have_object, FrameType::control, Bytes{1});
  auto failed = ask(scheduler, peer(1), 3, have_object, FrameType::control, Bytes{2});
  auto unknown = ask(scheduler, peer(1), 5, foreign, FrameType::control);
  for (auto* future : {&done, &failed, &unknown}) ASSERT_TRUE(answered_within(*future, 2s));
  EXPECT_EQ(done.get().type, message::ok);
  const auto refusal = refusal_of(failed.get());
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->status, 409);
  EXPECT_EQ(refusal->code, "conflict");
  EXPECT_EQ(refusal->message, "already there");
  const auto declined = refusal_of(unknown.get());
  ASSERT_TRUE(declined);
  EXPECT_EQ(declined->status, 501);
  EXPECT_EQ(declined->code, "not_implemented");
  for (int spin = 0; spin < 200 && scheduler.stats().message_timings.size() < 2; ++spin) std::this_thread::sleep_for(5ms);
  const auto stats = scheduler.stats();
  EXPECT_EQ(stats.message_timings.at(have_object).requests, 2u);
  EXPECT_EQ(stats.message_timings.at(have_object).failed, 1u);
  EXPECT_EQ(stats.message_timings.at(have_object).unrecognised, 0u);
  EXPECT_EQ(stats.message_timings.at(foreign).unrecognised, 1u);
}

// A notification has no one to answer; what the handler said is still
// counted.
TEST(Scheduler, AnUnrecognisedNotificationIsCountedAndAnswersNothing) {
  std::promise<void> ran;
  RequestScheduler scheduler(routes(), ServerLimits{}, [&](const membership::NodeRecord&, FrameType, const Message&) {
    ran.set_value();
    return Handled::unrecognised();
  });
  scheduler.submit(peer(1), Frame{0, FrameType::speculative, {MessageType{1998}, {}, {}}}, Reply{});
  ASSERT_EQ(ran.get_future().wait_for(2s), std::future_status::ready);
  for (int spin = 0; spin < 200 && scheduler.stats().message_timings.empty(); ++spin) std::this_thread::sleep_for(5ms);
  EXPECT_EQ(scheduler.stats().message_timings.at(MessageType{1998}).unrecognised, 1u);
}

// A throw is a failure: answered 500 and counted as one.
TEST(Scheduler, AThrowIsCountedAsAFailure) {
  RequestScheduler scheduler(routes(), ServerLimits{},
                             [](const membership::NodeRecord&, FrameType, const Message&) -> Handled { throw std::runtime_error("broken"); });
  auto broken = ask(scheduler, peer(1), 1, have_object, FrameType::control);
  ASSERT_TRUE(answered_within(broken, 2s));
  EXPECT_EQ(refusal_of(broken.get())->status, 500);
  for (int spin = 0; spin < 200 && scheduler.stats().message_timings.empty(); ++spin) std::this_thread::sleep_for(5ms);
  EXPECT_EQ(scheduler.stats().message_timings.at(have_object).failed, 1u);
}

// Held ordered work delays no other class.
TEST(Scheduler, HeldOrderedWorkDelaysNoOtherClass) {
  test::Gate gate;
  RequestScheduler scheduler(routes(), {}, [&](const membership::NodeRecord&, FrameType, const Message& request) {
    if (request.type == put_commit) gate.enter_and_wait();
    return Handled::processed({message::ok, {}, {}});
  });
  auto held = ask(scheduler, peer(1), 1, put_commit, FrameType::control);
  ASSERT_TRUE(gate.wait_for_entries(1));
  auto ping = ask(scheduler, peer(1), 2, message::ping, FrameType::control);
  auto have = ask(scheduler, peer(1), 3, have_object, FrameType::control);
  auto get = ask(scheduler, peer(1), 4, get_object, FrameType::foreground);
  EXPECT_TRUE(answered_within(ping, 5s));
  EXPECT_TRUE(answered_within(have, 5s));
  EXPECT_TRUE(answered_within(get, 5s));
  gate.open();
  EXPECT_TRUE(answered_within(held, 5s));
}

// One peer's control mutation is not queued behind that peer's own background
// mutations: Macha's order, 1, 5, 2, 3, 4.
TEST(Scheduler, AControlMutationOvertakesItsOwnPeersBackgroundWork) {
  test::Gate gate;
  std::mutex mutex;
  std::vector<uint8_t> order;
  RequestScheduler scheduler(routes(), {}, [&](const membership::NodeRecord&, FrameType, const Message& request) {
    {
      std::lock_guard lock(mutex);
      order.push_back(request.payload.at(0));
    }
    if (request.payload.at(0) == 1) gate.enter_and_wait();
    return Handled::processed({message::ok, {}, {}});
  });
  std::vector<std::future<Message>> answers;
  answers.push_back(ask(scheduler, peer(1), 1, put_commit, FrameType::loader, {1}));
  ASSERT_TRUE(gate.wait_for_entries(1));
  for (uint8_t n = 2; n <= 4; ++n) answers.push_back(ask(scheduler, peer(1), n, put_commit, FrameType::loader, {n}));
  answers.push_back(ask(scheduler, peer(1), 5, put_commit, FrameType::control, {5}));
  gate.open();
  for (auto& answer : answers) EXPECT_TRUE(answered_within(answer, 5s));
  std::lock_guard lock(mutex);
  EXPECT_EQ(order, (std::vector<uint8_t>{1, 5, 2, 3, 4}));
}

TEST(Scheduler, ATimingRecordsItsWaitAndHandlerTime) {
  test::SteppedTime time;
  test::Gate gate;
  RequestScheduler scheduler(
      routes(), {},
      [&](const membership::NodeRecord&, FrameType, const Message&) {
        time.advance(7ms);
        return Handled::processed({message::ok, {}, {}});
      },
      nullptr, &time);
  auto answer = ask(scheduler, peer(1), 1, have_object, FrameType::control);
  ASSERT_TRUE(answered_within(answer, 5s));
  ASSERT_TRUE([&] {
    for (int i = 0; i < 1000; ++i) {
      if (scheduler.stats().message_timings.contains(have_object)) return true;
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }());
  const auto timing = scheduler.stats().message_timings.at(have_object);
  EXPECT_EQ(timing.handler_us_total, 7000u);
  EXPECT_EQ(timing.handler_us_max, 7000u);
}

// Macha's rule for asking again: 429, 500, 502, 503, 504, and a refusal that names
// no status. Any other refusal stands.
TEST(Refusal, ATextOnlyErrorReadsAtStatusZeroAndMayBeAskedAgain) {
  Writer writer;
  writer.string("only text");
  const auto refusal = refusal_of({message::error, writer.take(), {}});
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->status, 0u);
  EXPECT_EQ(refusal->message, "only text");
  EXPECT_TRUE(try_later(*refusal));
  for (const int status : {429, 500, 502, 503, 504}) EXPECT_TRUE(try_later(Refusal{static_cast<uint16_t>(status), {}, {}})) << status;
  for (const int status : {400, 401, 403, 404, 409, 410, 413, 501, 507}) EXPECT_FALSE(try_later(Refusal{static_cast<uint16_t>(status), {}, {}})) << status;
}

// A type marked on_session is handled by the arrival itself: in order, ahead
// of every queue, and never refused, though every queue is full and every
// worker held. A handler that throws costs the session nothing.
TEST(Scheduler, AnOnSessionNotificationIsNeverQueuedOrRefused) {
  constexpr MessageType immediate{2100};
  auto table = RouteTable::with_dannan_types();
  table.add(put_commit, {"put_metadata_commit", Lanes::control, Frames::any, false, false, true});
  table.add(immediate, {"immediate", Lanes::both, Frames::any, true, false, false, std::nullopt, std::nullopt, true});
  auto shared = std::make_shared<const RouteTable>(std::move(table));
  test::Gate gate;
  std::mutex mutex;
  std::vector<uint8_t> seen;
  std::vector<std::thread::id> threads;
  ServerLimits limits;
  limits.ordered_pending_jobs = 2;
  RequestScheduler scheduler(shared, limits, [&](const membership::NodeRecord&, FrameType, const Message& message) {
    if (message.type != immediate) {
      gate.enter_and_wait();
      return Handled::processed();
    }
    if (message.payload.at(0) == 99) throw std::runtime_error("this handler fails");
    std::lock_guard lock(mutex);
    seen.push_back(message.payload.at(0));
    threads.push_back(std::this_thread::get_id());
    return Handled::processed();
  });
  std::vector<std::future<Message>> held;
  for (uint64_t i = 1; i <= 6; ++i) held.push_back(ask(scheduler, peer(1), i * 2, put_commit, FrameType::control));
  ASSERT_TRUE(gate.wait_for_entries(1));
  ASSERT_GT(scheduler.stats().ordered_rejected, 0u);
  for (const uint8_t i : std::vector<uint8_t>{1, 99, 2, 3}) scheduler.submit(peer(1), Frame{0, FrameType::loader, {immediate, Bytes{i}, {}}}, Reply{});
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(seen, (std::vector<uint8_t>{1, 2, 3}));
    for (const auto& thread : threads) EXPECT_EQ(thread, std::this_thread::get_id());
  }
  gate.open();
  for (auto& answer : held) EXPECT_TRUE(answered_within(answer, 10s));
  scheduler.stop();
}

// Only a notification may be marked: a request's reply would be written from
// the arrival, and a queue's rules do not apply to what never queues.
TEST(Routes, OnSessionIsOnlyForAPlainNotification) {
  auto table = RouteTable::with_dannan_types();
  EXPECT_THROW(table.add(MessageType{2101}, {"call", Lanes::control, Frames::any, false, false, false, std::nullopt, std::nullopt, true}),
               std::invalid_argument);
  EXPECT_THROW(table.add(MessageType{2102}, {"ordered", Lanes::control, Frames::any, true, false, true, std::nullopt, std::nullopt, true}),
               std::invalid_argument);
  EXPECT_THROW(table.add(MessageType{2103}, {"fast", Lanes::control, Frames::any, true, true, false, std::nullopt, std::nullopt, true}), std::invalid_argument);
  table.add(MessageType{2104}, {"plain", Lanes::control, Frames::any, true, false, false, std::nullopt, std::nullopt, true});
  EXPECT_TRUE(table.is_on_session(MessageType{2104}));
  EXPECT_FALSE(table.is_ordered(MessageType{2104}));
  EXPECT_TRUE(table.is_on_session(message::relay_data));
  EXPECT_TRUE(table.is_on_session(message::relay_credit));
  EXPECT_TRUE(table.is_on_session(message::relay_close));
}

}  // namespace
}  // namespace dannan::net
