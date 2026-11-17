//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// P4: the CPU pacer, the per-class gate and the worker pool, from the design
// (TODO/2026-11-14-p4-worker-pool.md) and laws 1 to 4: work runs by class, a
// background class is paced and never stopped, one gate per class however many
// forms its work runs in, and a task that cannot be served is told so at once.

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dannan/sched/class_gate.h"
#include "dannan/sched/cpu_pacing.h"
#include "dannan/sched/worker_pool.h"
#include "dannan/test/fixtures/gate.h"
#include "dannan/test/fixtures/stepped_time.h"

namespace dannan::sched {
namespace {

using namespace std::chrono_literals;

bool eventually(const std::function<bool()>& condition, std::chrono::milliseconds bound = 10s) {
  const auto until = std::chrono::steady_clock::now() + bound;
  while (std::chrono::steady_clock::now() < until) {
    if (condition()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return condition();
}

// An executor for completions, run on a thread of its own.
struct Executor {
  boost::asio::io_context io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{io.get_executor()};
  std::thread thread{[this] { io.run(); }};
  ~Executor() {
    work.reset();
    thread.join();
  }
};

// The process's CPU, as the test sets it.
struct FakeCpu {
  std::atomic<double> process{0.0};
  std::atomic<double> thread{0.0};
  ProcessCpu process_source() {
    return [this] { return process.load(); };
  }
  ThreadCpu thread_source() {
    return [this] { return thread.load(); };
  }
};

GateConfig even_share() {
  GateConfig config;
  config.higher_weight = 1;
  config.own_weight = 1;
  config.slice = 1000ms;
  return config;
}

WorkerPoolConfig one_thread() {
  WorkerPoolConfig config;
  config.threads = 1;
  config.reserve = {0, 0, 0, 0};
  return config;
}

WorkContext of(FrameType type, TimePoint deadline = {}, std::atomic_bool* cancelled = nullptr) { return WorkContext(type, deadline, cancelled, "test"); }

// --- The pacer, as Dagda's -------------------------------------------------

TEST(CpuPacing, KeepsTheWholeBudgetAtOrUnderTheTargetAndScalesAbove) {
  EXPECT_DOUBLE_EQ(background_cpu_scale(0.5, 0.0, 1.0, 0.5), 1.0);
  EXPECT_DOUBLE_EQ(background_cpu_scale(1.0, 0.0, 1.0, 0.5), 0.5);
  // The pacing thread's own use is taken out.
  EXPECT_DOUBLE_EQ(background_cpu_scale(1.0, 0.5, 1.0, 0.5), 1.0);
  EXPECT_DOUBLE_EQ(background_cpu_scale(1.0, 0.0, 0.0, 0.5), 1.0);
  CpuSample sample;
  CpuPacer pacer([&] { return sample; });
  sample.process_seconds = 2.0;
  EXPECT_DOUBLE_EQ(pacer.scale(1.0, 0.5), 0.25);
  EXPECT_DOUBLE_EQ(CpuPacer({}).scale(1.0, 0.5), 1.0);
  const auto real = system_cpu_sample();
  EXPECT_GE(real.process_seconds, 0.0);
  EXPECT_GE(real.own_seconds, 0.0);
}

// --- The gate ---------------------------------------------------------------

TEST(ClassGate, PacesOnlyTheBackgroundClasses) {
  EXPECT_THROW((void)ClassGate(WorkClass::control), std::invalid_argument);
  EXPECT_THROW((void)ClassGate(WorkClass::foreground), std::invalid_argument);
  GateConfig bad;
  bad.cpu_target = 0.0;
  EXPECT_THROW((void)ClassGate(WorkClass::loader, bad), std::invalid_argument);
  ClassGates gates;
  EXPECT_EQ(gates.gate(WorkClass::loader).work(), WorkClass::loader);
  EXPECT_EQ(gates.gate(WorkClass::speculative).work(), WorkClass::speculative);
  EXPECT_THROW(gates.gate(WorkClass::foreground), std::invalid_argument);
}

// Contended while work above is queued or running, or the host says so: foreground
// contends with both background classes, loader with speculative only.
TEST(ClassGate, ContendsWhileWorkAboveIsPresentOrTheHostSays) {
  ClassGates gates;
  auto& loader = gates.gate(WorkClass::loader);
  auto& speculative = gates.gate(WorkClass::speculative);
  EXPECT_FALSE(loader.contended());
  gates.note(WorkClass::loader, +1);
  EXPECT_FALSE(loader.contended());
  EXPECT_TRUE(speculative.contended());
  gates.note(WorkClass::loader, -1);
  gates.note(WorkClass::foreground, +1);
  EXPECT_TRUE(loader.contended());
  EXPECT_TRUE(speculative.contended());
  gates.note(WorkClass::foreground, -1);
  EXPECT_FALSE(speculative.contended());
  std::atomic<bool> viewers{true};
  speculative.set_contended([&] { return viewers.load(); });
  EXPECT_TRUE(speculative.contended());
  viewers = false;
  EXPECT_FALSE(speculative.contended());
}

// The hosted reading may be set, replaced and cleared while the gate is read on
// other threads (Macha's review: its reading exists only after its store recovers).
TEST(ClassGate, TheHostedReadingMayChangeWhileInUse) {
  ClassGate gate(WorkClass::speculative);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0};
  std::thread reader([&] {
    while (!stop.load()) {
      (void)gate.contended();
      ++reads;
    }
  });
  for (int i = 0; i < 200; ++i) {
    gate.set_contended([] { return true; });
    gate.set_contended([] { return false; });
    gate.set_contended({});
  }
  EXPECT_TRUE(eventually([&] { return reads.load() > 0; }));
  stop = true;
  reader.join();
  EXPECT_FALSE(gate.contended());
  gate.set_contended([] { return true; });
  EXPECT_TRUE(gate.contended());
  gate.set_contended({});
  EXPECT_FALSE(gate.contended());
}

// Once a replacement returns, the old reading is never running and never called
// again, so what it reads may go (Macha's review: its stop clears the reading, then
// destroys the store it reads).
TEST(ClassGate, AReplacedReadingIsNeverCalledAfterTheReplacementReturns) {
  ClassGate gate(WorkClass::speculative);
  std::atomic<bool> cleared{false};
  std::atomic<bool> late{false};
  std::atomic<int> entered{0};
  gate.set_contended([&] {
    ++entered;
    std::this_thread::sleep_for(20ms);
    if (cleared.load()) late = true;
    return true;
  });
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop.load()) (void)gate.contended();
  });
  ASSERT_TRUE(eventually([&] { return entered.load() > 0; }));
  gate.set_contended({});
  cleared = true;
  std::this_thread::sleep_for(60ms);
  stop = true;
  reader.join();
  EXPECT_FALSE(late.load());
  EXPECT_FALSE(gate.contended());
}

// A loop's burst while contended earns a rest by the weights; uncontended there is none.
TEST(ClassGate, ALoopsBurstEarnsItsRest) {
  test::SteppedTime time;
  ClassGate gate(WorkClass::speculative, even_share(), &time);
  gate.started();
  time.advance(10ms);
  EXPECT_EQ(gate.finished(), 0ms);
  gate.set_contended([] { return true; });
  EXPECT_TRUE(gate.may_start());
  gate.started();
  time.advance(10ms);
  EXPECT_FALSE(gate.should_yield());
  EXPECT_EQ(gate.finished(), 10ms);
  EXPECT_FALSE(gate.may_start());
  EXPECT_EQ(gate.rest(), 10ms);
  time.advance(10ms);
  EXPECT_TRUE(gate.may_start());
  EXPECT_EQ(gate.rest(), 0ms);
}

// The scale is the process's load less what the class charged, against the target,
// read at most once a window; above the target a task's CPU earns a rest.
TEST(ClassGate, ScalesByTheLoadOfEverythingElseAndRestsTasksByIt) {
  test::SteppedTime time;
  FakeCpu cpu;
  ClassGate gate(WorkClass::speculative, {}, &time, cpu.process_source());
  EXPECT_DOUBLE_EQ(gate.cpu_scale(), 1.0);
  time.advance(1s);
  cpu.process = 1.0;
  EXPECT_DOUBLE_EQ(gate.cpu_scale(), 0.5);
  // Within the window the reading stands.
  cpu.process = 1.5;
  EXPECT_DOUBLE_EQ(gate.cpu_scale(), 0.5);
  time.advance(1s);
  cpu.process = 2.25;
  gate.charge(0.75);
  // (2.25 - 1.0 - 0.75) / 1 s is the target: the whole budget.
  EXPECT_DOUBLE_EQ(gate.cpu_scale(), 1.0);
  EXPECT_EQ(gate.charge_and_rest(0.1), 0ms);
  time.advance(1s);
  cpu.process = 3.45;
  // (3.45 - 2.25 - 0.1 - 0.1) / 1 s = 1.0 against 0.5: half, so 0.1 s earns 0.1 s.
  EXPECT_EQ(gate.charge_and_rest(0.1), 100ms);
  EXPECT_EQ(gate.rest(), 100ms);
  time.advance(100ms);
  EXPECT_EQ(gate.rest(), 0ms);
}

// Never stopped: however loaded the process, the scale keeps its floor.
TEST(ClassGate, TheScaleHasAFloor) {
  test::SteppedTime time;
  FakeCpu cpu;
  ClassGate gate(WorkClass::loader, {}, &time, cpu.process_source());
  (void)gate.cpu_scale();
  time.advance(1s);
  cpu.process = 1000.0;
  EXPECT_DOUBLE_EQ(gate.cpu_scale(), 0.05);
}

// --- The pool ---------------------------------------------------------------

// Control first, then foreground, loader and speculative, first in first out within a class.
TEST(WorkerPool, RunsByClassControlFirst) {
  ClassGates gates;
  WorkerPool pool(one_thread(), gates);
  pool.start();
  test::Gate held;
  test::GateOpener opener(held);
  std::mutex mutex;
  std::vector<std::string> order;
  const auto record = [&](std::string name) {
    return [&, name](WorkerPool::Turn&) {
      std::lock_guard lock(mutex);
      order.push_back(name);
    };
  };
  ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { held.enter_and_wait(); }).accepted);
  ASSERT_TRUE(held.wait_for_entries(1));
  for (const auto& [type, name] : std::vector<std::pair<FrameType, std::string>>{{FrameType::speculative, "speculative"},
                                                                                 {FrameType::loader, "loader"},
                                                                                 {FrameType::foreground, "foreground 1"},
                                                                                 {FrameType::control, "control"},
                                                                                 {FrameType::read_ahead, "foreground 2"}})
    ASSERT_TRUE(pool.submit(of(type), record(name)).accepted);
  held.open();
  ASSERT_TRUE(eventually([&] {
    std::lock_guard lock(mutex);
    return order.size() == 5;
  }));
  EXPECT_EQ(order, (std::vector<std::string>{"control", "foreground 1", "foreground 2", "loader", "speculative"}));
}

// On a schedule (the simulation harness's, WorkerPoolConfig::schedule): no threads; nothing runs
// until the schedule runs a turn, each turn runs one task, by class as the threads would, and the
// next turn is booked while work waits. Stopped, a booked turn does nothing.
TEST(WorkerPool, OnAScheduleRunsOneTaskATurnByClassWithNoThreads) {
  std::vector<std::function<void()>> turns;
  auto config = one_thread();
  config.schedule = [&](std::chrono::milliseconds, std::function<void()> turn) { turns.push_back(std::move(turn)); };
  ClassGates gates;
  WorkerPool pool(config, gates);
  std::vector<std::string> order;
  const auto record = [&](std::string name) { return [&, name](WorkerPool::Turn&) { order.push_back(name); }; };
  for (const auto& [type, name] : std::vector<std::pair<FrameType, std::string>>{
           {FrameType::speculative, "speculative"}, {FrameType::foreground, "foreground"}, {FrameType::control, "control"}})
    ASSERT_TRUE(pool.submit(of(type), record(name)).accepted);
  EXPECT_TRUE(turns.empty());
  pool.start();
  ASSERT_EQ(turns.size(), 1u);
  EXPECT_TRUE(order.empty());
  // Each turn runs one task and books the next while any waits.
  while (!turns.empty()) {
    auto turn = std::move(turns.front());
    turns.erase(turns.begin());
    turn();
  }
  EXPECT_EQ(order, (std::vector<std::string>{"control", "foreground", "speculative"}));
  // A task submitted later books a turn of its own.
  ASSERT_TRUE(pool.submit(of(FrameType::control), record("later")).accepted);
  ASSERT_EQ(turns.size(), 1u);
  pool.stop();
  turns.front()();
  EXPECT_EQ(order.back(), "speculative");
}

// A full queue refuses at once and says so; a stopped pool refuses as stopped.
TEST(WorkerPool, RefusesAtOnceWhenItsQueueIsFull) {
  ClassGates gates;
  auto config = one_thread();
  config.queue_bound[static_cast<size_t>(WorkClass::foreground)] = 1;
  WorkerPool pool(config, gates);
  pool.start();
  test::Gate held;
  test::GateOpener opener(held);
  ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { held.enter_and_wait(); }).accepted);
  ASSERT_TRUE(held.wait_for_entries(1));
  EXPECT_TRUE(pool.submit(of(FrameType::foreground), [](WorkerPool::Turn&) {}).accepted);
  const auto refused = pool.submit(of(FrameType::foreground), [](WorkerPool::Turn&) {});
  EXPECT_FALSE(refused.accepted);
  EXPECT_EQ(refused.why, WorkerPool::Refused::queue_full);
  EXPECT_STREQ(refused_name(refused.why), "queue_full");
  // Another class's queue is its own.
  EXPECT_TRUE(pool.submit(of(FrameType::loader), [](WorkerPool::Turn&) {}).accepted);
  EXPECT_EQ(pool.stats(WorkClass::foreground).refused_full, 1u);
  held.open();
  pool.stop();
  const auto stopped = pool.submit(of(FrameType::foreground), [](WorkerPool::Turn&) {});
  EXPECT_EQ(stopped.why, WorkerPool::Refused::stopped);
  EXPECT_THROW(pool.submit(of(FrameType::foreground), [](WorkerPool::Turn&) {}, {}, [](WorkerPool::Ended) {}), std::invalid_argument);
}

// A class's cap holds the rest of its tasks in the queue, with workers free.
TEST(WorkerPool, ACapLimitsHowManyOfAClassRunAtOnce) {
  ClassGates gates;
  WorkerPoolConfig config;
  config.threads = 3;
  config.reserve = {0, 0, 0, 0};
  config.max_running[static_cast<size_t>(WorkClass::foreground)] = 1;
  WorkerPool pool(config, gates);
  pool.start();
  test::Gate held;
  test::GateOpener opener(held);
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { held.enter_and_wait(); }).accepted);
  ASSERT_TRUE(held.wait_for_entries(1));
  EXPECT_FALSE(held.wait_for_entries(2, 200ms));
  EXPECT_EQ(pool.stats(WorkClass::foreground).running, 1u);
  EXPECT_EQ(pool.stats(WorkClass::foreground).queued, 2u);
  held.open();
  EXPECT_TRUE(eventually([&] { return pool.stats(WorkClass::foreground).ran == 3; }));
}

// A worker held back for foreground: blocked speculative tasks never take it.
TEST(WorkerPool, AReserveKeepsAWorkerForTheClassAbove) {
  ClassGates gates;
  WorkerPoolConfig config;
  config.threads = 2;
  config.reserve = {0, 1, 0, 0};
  WorkerPool pool(config, gates);
  pool.start();
  test::Gate held;
  test::GateOpener opener(held);
  for (int i = 0; i < 2; ++i) ASSERT_TRUE(pool.submit(of(FrameType::speculative), [&](WorkerPool::Turn&) { held.enter_and_wait(); }).accepted);
  ASSERT_TRUE(held.wait_for_entries(1));
  EXPECT_FALSE(held.wait_for_entries(2, 200ms));
  std::atomic<bool> served{false};
  ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { served = true; }).accepted);
  EXPECT_TRUE(eventually([&] { return served.load(); }));
  held.open();
  EXPECT_TRUE(eventually([&] { return pool.stats(WorkClass::speculative).ran == 2; }));
  const auto sized = [](size_t threads, PerClass<size_t> reserve) {
    WorkerPoolConfig out;
    out.threads = threads;
    out.reserve = reserve;
    return out;
  };
  // Reserves that leave no worker for speculative work, and no threads at all.
  EXPECT_THROW((void)WorkerPool(sized(2, {1, 1, 0, 0}), gates), std::invalid_argument);
  EXPECT_THROW((void)WorkerPool(sized(0, {0, 0, 0, 0}), gates), std::invalid_argument);
}

// A task whose deadline passes, or which is cancelled, while it waits is dropped
// unrun, and its caller is told so on its executor.
TEST(WorkerPool, QueuedWorkPastItsDeadlineOrCancelledIsDroppedAndTold) {
  // Before the pool: the executor must outlive the pool that posts to it.
  Executor executor;
  test::SteppedTime time;
  ClassGates gates({}, {}, &time);
  WorkerPool pool(one_thread(), gates, nullptr, &time);
  pool.start();
  test::Gate held;
  test::GateOpener opener(held);
  ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { held.enter_and_wait(); }).accepted);
  ASSERT_TRUE(held.wait_for_entries(1));
  std::atomic<int> ran{0};
  std::promise<WorkerPool::Ended> late_end, cancelled_end;
  std::atomic_bool cancel{false};
  ASSERT_TRUE(pool.submit(
                      of(FrameType::foreground, time.now() + 50ms), [&](WorkerPool::Turn&) { ++ran; }, executor.io.get_executor(),
                      [&](WorkerPool::Ended ended) { late_end.set_value(ended); })
                  .accepted);
  ASSERT_TRUE(pool.submit(
                      of(FrameType::foreground, {}, &cancel), [&](WorkerPool::Turn&) { ++ran; }, executor.io.get_executor(),
                      [&](WorkerPool::Ended ended) { cancelled_end.set_value(ended); })
                  .accepted);
  time.advance(100ms);
  cancel = true;
  held.open();
  auto late = late_end.get_future();
  auto cancelled = cancelled_end.get_future();
  ASSERT_EQ(late.wait_for(10s), std::future_status::ready);
  ASSERT_EQ(cancelled.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(late.get().how, WorkerPool::End::past_deadline);
  EXPECT_EQ(cancelled.get().how, WorkerPool::End::cancelled);
  EXPECT_EQ(ran.load(), 0);
  EXPECT_EQ(pool.stats(WorkClass::foreground).dropped, 2u);
}

// While the work above contends, a speculative task's burst earns a rest by the
// weights: the next waits it out, and then runs. Paced, never stopped.
TEST(WorkerPool, BackgroundWorkIsPacedNotStoppedWhileTheWorkAboveContends) {
  test::SteppedTime time;
  FakeCpu cpu;
  ClassGates gates({}, even_share(), &time, cpu.process_source());
  gates.gate(WorkClass::speculative).set_contended([] { return true; });
  WorkerPool pool(one_thread(), gates, nullptr, &time, cpu.thread_source());
  pool.start();
  std::atomic<int> ran{0};
  ASSERT_TRUE(pool.submit(of(FrameType::speculative),
                          [&](WorkerPool::Turn&) {
                            time.advance(20ms);
                            ++ran;
                          })
                  .accepted);
  ASSERT_TRUE(eventually([&] { return pool.stats(WorkClass::speculative).ran == 1; }));
  ASSERT_TRUE(pool.submit(of(FrameType::speculative), [&](WorkerPool::Turn&) { ++ran; }).accepted);
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(ran.load(), 1);
  time.advance(20ms);
  EXPECT_TRUE(eventually([&] { return ran.load() == 2; }));
}

// Above the CPU target, a background task rests by the CPU it used, its thread's
// CPU time and not the wall time it took.
TEST(WorkerPool, ABackgroundTaskRestsByTheCpuItUsed) {
  test::SteppedTime time;
  FakeCpu cpu;
  ClassGates gates({}, {}, &time, cpu.process_source());
  WorkerPool pool(one_thread(), gates, nullptr, &time, cpu.thread_source());
  (void)gates.gate(WorkClass::loader).cpu_scale();
  time.advance(1s);
  // The process ran 1.1 s in the second; the task's 0.1 s is its own: load 1.0
  // against 0.5, so half the budget, and 0.1 s earns 0.1 s.
  cpu.process = 1.1;
  pool.start();
  std::atomic<int> ran{0};
  ASSERT_TRUE(pool.submit(of(FrameType::loader),
                          [&](WorkerPool::Turn&) {
                            cpu.thread = cpu.thread.load() + 0.1;
                            ++ran;
                          })
                  .accepted);
  ASSERT_TRUE(eventually([&] { return pool.stats(WorkClass::loader).ran == 1; }));
  ASSERT_TRUE(pool.submit(of(FrameType::loader), [&](WorkerPool::Turn&) { ++ran; }).accepted);
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(ran.load(), 1);
  EXPECT_EQ(gates.gate(WorkClass::loader).rest(), 100ms);
  time.advance(100ms);
  EXPECT_TRUE(eventually([&] { return ran.load() == 2; }));
  EXPECT_EQ(pool.stats(WorkClass::loader).rested, 100ms);
}

// A loop holding the gate and the pool's tasks draw on one share: the rest the
// loop earned holds the pool's task back too.
TEST(WorkerPool, ALoopAndThePoolShareOneGate) {
  test::SteppedTime time;
  FakeCpu cpu;
  ClassGates gates({}, even_share(), &time, cpu.process_source());
  auto& gate = gates.gate(WorkClass::speculative);
  gate.set_contended([] { return true; });
  WorkerPool pool(one_thread(), gates, nullptr, &time, cpu.thread_source());
  pool.start();
  gate.started();
  time.advance(30ms);
  ASSERT_EQ(gate.finished(), 30ms);
  std::atomic<bool> ran{false};
  ASSERT_TRUE(pool.submit(of(FrameType::speculative), [&](WorkerPool::Turn&) { ran = true; }).accepted);
  std::this_thread::sleep_for(150ms);
  EXPECT_FALSE(ran.load());
  time.advance(30ms);
  EXPECT_TRUE(eventually([&] { return ran.load(); }));
}

// Foreground work queued in the pool contends with the background gates until it ends.
TEST(WorkerPool, QueuedForegroundContendsWithTheBackgroundGates) {
  ClassGates gates;
  WorkerPool pool(one_thread(), gates);
  pool.start();
  test::Gate held;
  test::GateOpener opener(held);
  ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { held.enter_and_wait(); }).accepted);
  ASSERT_TRUE(held.wait_for_entries(1));
  EXPECT_TRUE(gates.gate(WorkClass::speculative).contended());
  EXPECT_TRUE(gates.gate(WorkClass::loader).contended());
  held.open();
  EXPECT_TRUE(eventually([&] { return !gates.gate(WorkClass::speculative).contended(); }));
}

// A background task asked to yield at an operation boundary once its slice is spent.
TEST(WorkerPool, ABackgroundTaskIsAskedToYieldAtItsSlicesEnd) {
  test::SteppedTime time;
  FakeCpu cpu;
  ClassGates gates({}, even_share(), &time, cpu.process_source());
  gates.gate(WorkClass::speculative).set_contended([] { return true; });
  WorkerPool pool(one_thread(), gates, nullptr, &time, cpu.thread_source());
  pool.start();
  std::promise<std::pair<bool, bool>> asked;
  ASSERT_TRUE(pool.submit(of(FrameType::speculative),
                          [&](WorkerPool::Turn& turn) {
                            const bool early = turn.should_yield();
                            time.advance(1000ms);
                            asked.set_value({early, turn.should_yield()});
                          })
                  .accepted);
  auto answer = asked.get_future();
  ASSERT_EQ(answer.wait_for(10s), std::future_status::ready);
  const auto [early, late] = answer.get();
  EXPECT_FALSE(early);
  EXPECT_TRUE(late);
}

// A task and its completion may own what cannot be copied, as a password's Secret
// is (Lugh's review).
TEST(WorkerPool, ATaskAndItsCompletionMayOwnWhatCannotBeCopied) {
  Executor executor;
  ClassGates gates;
  WorkerPool pool(one_thread(), gates);
  pool.start();
  auto secret = std::make_unique<int>(7);
  auto witness = std::make_unique<int>(9);
  std::promise<int> seen;
  std::promise<int> told;
  ASSERT_TRUE(pool.submit(
                      of(FrameType::foreground), [owned = std::move(secret), &seen](WorkerPool::Turn&) { seen.set_value(*owned); }, executor.io.get_executor(),
                      [owned = std::move(witness), &told](WorkerPool::Ended ended) { told.set_value(ended.how == WorkerPool::End::ran ? *owned : -1); })
                  .accepted);
  auto ran = seen.get_future();
  auto ended = told.get_future();
  ASSERT_EQ(ran.wait_for(10s), std::future_status::ready);
  ASSERT_EQ(ended.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(ran.get(), 7);
  EXPECT_EQ(ended.get(), 9);
}

// A task that throws is told as faulted, and the pool goes on.
TEST(WorkerPool, AThrowingTaskIsToldAndThePoolGoesOn) {
  Executor executor;
  ClassGates gates;
  WorkerPool pool(one_thread(), gates);
  pool.start();
  std::promise<WorkerPool::Ended> ended;
  ASSERT_TRUE(pool.submit(
                      of(FrameType::foreground), [](WorkerPool::Turn&) { throw std::runtime_error("broke"); }, executor.io.get_executor(),
                      [&](WorkerPool::Ended end) { ended.set_value(end); })
                  .accepted);
  auto end = ended.get_future();
  ASSERT_EQ(end.wait_for(10s), std::future_status::ready);
  const auto told = end.get();
  EXPECT_EQ(told.how, WorkerPool::End::faulted);
  EXPECT_EQ(told.fault, "broke");
  std::atomic<bool> next{false};
  ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { next = true; }).accepted);
  EXPECT_TRUE(eventually([&] { return next.load(); }));
  EXPECT_EQ(pool.stats(WorkClass::foreground).faulted, 1u);
}

// Stopping ends what is queued as stopped, told on each caller's executor.
TEST(WorkerPool, StoppingEndsQueuedTasksAsStopped) {
  Executor executor;
  ClassGates gates;
  WorkerPool pool(one_thread(), gates);
  pool.start();
  test::Gate held;
  test::GateOpener opener(held);
  ASSERT_TRUE(pool.submit(of(FrameType::foreground), [&](WorkerPool::Turn&) { held.enter_and_wait(); }).accepted);
  ASSERT_TRUE(held.wait_for_entries(1));
  std::promise<WorkerPool::Ended> ended;
  ASSERT_TRUE(
      pool.submit(of(FrameType::loader), [](WorkerPool::Turn&) {}, executor.io.get_executor(), [&](WorkerPool::Ended end) { ended.set_value(end); }).accepted);
  std::thread stopper([&] { pool.stop(); });
  auto end = ended.get_future();
  ASSERT_EQ(end.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(end.get().how, WorkerPool::End::stopped);
  held.open();
  stopper.join();
  EXPECT_STREQ(end_name(WorkerPool::End::stopped), "stopped");
}

}  // namespace
}  // namespace dannan::sched
