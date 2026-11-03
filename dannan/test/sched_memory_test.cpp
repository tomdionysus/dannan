//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The retained-memory ledger and the memory limit: the inventory's row
// "three memory reserves plus the reassembly reserve inside one retained
// total; a memory limit shrinks the configured parts pro rata with a MEMORY
// OVER LIMIT warning"; laws 1 to 4, discipline 5.

#include <gtest/gtest.h>

#include <atomic>
#include <stdexcept>
#include <thread>

#include "dannan/sched/memory_limit.h"
#include "dannan/sched/retained_memory.h"
#include "dannan/test/fixtures/gate.h"

namespace dannan::sched {
namespace {

constexpr auto hosted = static_cast<MemoryOwner>(256);

// 1000 bytes: control reserve 100, foreground 200, loader 100, reassembly 50.
RetainedMemoryLedger ledger() { return RetainedMemoryLedger(1000, 100, 200, 100, 50); }

TEST(RetainedMemory, RefusesReservesThatDoNotFitTheTotal) {
  EXPECT_THROW(RetainedMemoryLedger(0, 1, 1, 1, 1), std::invalid_argument);
  EXPECT_THROW(RetainedMemoryLedger(100, 0, 1, 1, 1), std::invalid_argument);
  EXPECT_THROW(RetainedMemoryLedger(100, 50, 40, 20, 1), std::invalid_argument);
  EXPECT_THROW(RetainedMemoryLedger(100, 10, 10, 10, 0), std::invalid_argument);
  EXPECT_THROW(RetainedMemoryLedger(100, 50, 30, 20, 1), std::invalid_argument);  // no room left for reassembly
  EXPECT_NO_THROW(RetainedMemoryLedger(100, 50, 30, 10, 1));
}

// Law 1: control may use the whole total; nothing else may touch its reserve.
TEST(RetainedMemory, ControlAloneMayUseItsReserve) {
  auto memory = ledger();
  auto foreground = memory.try_acquire(WorkClass::foreground, hosted, 900);
  ASSERT_TRUE(foreground);
  EXPECT_FALSE(memory.try_acquire(WorkClass::foreground, hosted, 1));
  auto control = memory.try_acquire(WorkClass::control, hosted, 100);
  EXPECT_TRUE(control);
  EXPECT_FALSE(memory.try_acquire(WorkClass::control, hosted, 1));
}

// Law 2: loader and speculative never take the foreground reserve.
TEST(RetainedMemory, LowerClassesLeaveTheForegroundReserve) {
  auto memory = ledger();
  auto loader = memory.try_acquire(WorkClass::loader, hosted, 700);
  ASSERT_TRUE(loader);
  EXPECT_FALSE(memory.try_acquire(WorkClass::loader, hosted, 1));
  auto foreground = memory.try_acquire(WorkClass::foreground, hosted, 200);
  EXPECT_TRUE(foreground);
}

// Law 3: speculative never takes the loader reserve.
TEST(RetainedMemory, SpeculativeLeavesTheLoaderReserve) {
  auto memory = ledger();
  auto speculative = memory.try_acquire(WorkClass::speculative, hosted, 600);
  ASSERT_TRUE(speculative);
  EXPECT_FALSE(memory.try_acquire(WorkClass::speculative, hosted, 1));
  auto loader = memory.try_acquire(WorkClass::loader, hosted, 100);
  EXPECT_TRUE(loader);
}

TEST(RetainedMemory, ALeaseReleasesItsBytesWhenItGoes) {
  auto memory = ledger();
  {
    auto lease = memory.try_acquire(WorkClass::foreground, hosted, 300);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->bytes(), 300u);
    EXPECT_EQ(memory.stats().used_bytes, 300u);
    auto moved = std::move(*lease);
    EXPECT_EQ(memory.stats().used_bytes, 300u);
  }
  EXPECT_EQ(memory.stats().used_bytes, 0u);
  EXPECT_EQ(memory.stats().peak_used_bytes, 300u);
}

TEST(RetainedMemory, AZeroByteRequestIsChargedOneByte) {
  auto memory = ledger();
  auto lease = memory.try_acquire(WorkClass::loader, hosted, 0);
  ASSERT_TRUE(lease);
  EXPECT_EQ(memory.stats().used_bytes, 1u);
}

TEST(RetainedMemory, CountsBytesPerOwner) {
  auto memory = ledger();
  auto a = memory.try_acquire(WorkClass::loader, hosted, 10);
  auto b = memory.try_acquire(WorkClass::loader, MemoryOwner::rpc_frame, 20);
  const auto stats = memory.stats();
  EXPECT_EQ(stats.owner_bytes.at(hosted), 10u);
  EXPECT_EQ(stats.owner_bytes.at(MemoryOwner::rpc_frame), 20u);
}

// Inbound reassembly gets a small reserve ahead of the lower-class gates and
// is exempt from the durable-lower budget: it is what releases that budget,
// since publication holds its bytes until a peer's confirmation is
// reassembled. Control's reserve still binds it.
TEST(RetainedMemory, ReassemblyIsNeverHeldBehindTheDurableLowerBudget) {
  auto memory = ledger();
  auto loader = memory.try_acquire(WorkClass::loader, hosted, 700);
  ASSERT_TRUE(loader);
  EXPECT_FALSE(memory.try_acquire(WorkClass::loader, hosted, 1));
  auto frame = memory.try_acquire(WorkClass::loader, MemoryOwner::rpc_frame, 200);
  EXPECT_TRUE(frame);
  EXPECT_FALSE(memory.try_acquire(WorkClass::loader, MemoryOwner::rpc_frame, 1));
}

// The reassembly reserve sits below the waiter gate: while foreground work
// waits, reassembly stands aside like any other lower-class work (law 2).
TEST(RetainedMemory, ReassemblyStandsAsideForWaitingForegroundWork) {
  auto memory = ledger();
  auto foreground = memory.try_acquire(WorkClass::foreground, hosted, 850);
  ASSERT_TRUE(foreground);
  std::thread waiter([&] { EXPECT_TRUE(memory.acquire(WorkClass::foreground, hosted, 100, {})); });
  while (memory.stats().waits[static_cast<size_t>(WorkClass::foreground)] == 0) std::this_thread::yield();
  EXPECT_FALSE(memory.try_acquire(WorkClass::loader, MemoryOwner::rpc_frame, 10));
  foreground.reset();
  waiter.join();
  EXPECT_TRUE(memory.try_acquire(WorkClass::loader, MemoryOwner::rpc_frame, 10));
}

// Lower classes stand aside while a higher one is waiting.
TEST(RetainedMemory, ALowerClassDoesNotOvertakeAWaitingHigherOne) {
  auto memory = ledger();
  auto foreground = memory.try_acquire(WorkClass::foreground, hosted, 850);
  ASSERT_TRUE(foreground);
  std::atomic_bool waiting{false};
  std::thread waiter([&] {
    waiting = true;
    auto lease = memory.acquire(WorkClass::foreground, hosted, 100, {});
    EXPECT_TRUE(lease);
  });
  while (memory.stats().waits[static_cast<size_t>(WorkClass::foreground)] == 0) std::this_thread::yield();
  EXPECT_FALSE(memory.try_acquire(WorkClass::loader, hosted, 10));
  foreground.reset();
  waiter.join();
}

// Reclaimable memory may borrow idle headroom only if it can give it back: a
// higher class that would wait asks for it, and the lower holder sheds.
TEST(RetainedMemory, AHigherClassMakesReclaimableLowerMemoryShed) {
  auto memory = ledger();
  std::optional<RetainedMemoryLedger::Lease> cache;
  cache = memory.try_acquire(WorkClass::speculative, hosted, 800, true, [&] { cache.reset(); });
  ASSERT_TRUE(cache);
  auto foreground = memory.acquire(WorkClass::foreground, hosted, 500, {});
  EXPECT_TRUE(foreground);
  EXPECT_FALSE(cache.has_value());
  EXPECT_EQ(memory.stats().shed_requests, 1u);
}

TEST(RetainedMemory, ReclaimableMemoryNeedsAShedCallback) {
  auto memory = ledger();
  EXPECT_THROW(memory.try_acquire(WorkClass::loader, hosted, 10, true), std::invalid_argument);
}

// Recovery of acknowledged work is never refused; the overcommit blocks new
// admissions until it drains.
TEST(RetainedMemory, RestoreIsNeverRefusedAndBlocksNewAdmissions) {
  auto memory = ledger();
  auto restored = memory.restore(WorkClass::loader, hosted, 1500);
  EXPECT_EQ(memory.stats().used_bytes, 1500u);
  EXPECT_EQ(memory.stats().restored_bytes, 1500u);
  EXPECT_FALSE(memory.try_acquire(WorkClass::control, hosted, 1));
  restored = RetainedMemoryLedger::Lease{};
  EXPECT_TRUE(memory.try_acquire(WorkClass::control, hosted, 1));
}

TEST(RetainedMemory, AWaitEndsOnCancellation) {
  auto memory = ledger();
  auto all = memory.try_acquire(WorkClass::control, hosted, 1000);
  ASSERT_TRUE(all);
  std::atomic_bool cancelled{false};
  std::optional<RetainedMemoryLedger::Lease> got;
  bool returned = false;
  std::thread waiter([&] {
    got = memory.acquire(WorkClass::loader, hosted, 10, {}, &cancelled);
    returned = true;
  });
  while (memory.stats().waits[static_cast<size_t>(WorkClass::loader)] == 0) std::this_thread::yield();
  cancelled = true;
  memory.wake();
  waiter.join();
  EXPECT_TRUE(returned);
  EXPECT_FALSE(got.has_value());
  EXPECT_EQ(memory.stats().cancelled_waits, 1u);
}

TEST(RetainedMemory, AWaitEndsAtItsDeadline) {
  auto memory = ledger();
  auto all = memory.try_acquire(WorkClass::control, hosted, 1000);
  ASSERT_TRUE(all);
  EXPECT_FALSE(memory.acquire(WorkClass::loader, hosted, 10, Clock::now() + std::chrono::milliseconds(20)));
}

TEST(RetainedMemory, StoppingReleasesEveryWaiterEmpty) {
  auto memory = ledger();
  auto all = memory.try_acquire(WorkClass::control, hosted, 1000);
  ASSERT_TRUE(all);
  std::optional<RetainedMemoryLedger::Lease> got;
  std::thread waiter([&] { got = memory.acquire(WorkClass::foreground, hosted, 10, {}); });
  while (memory.stats().waits[static_cast<size_t>(WorkClass::foreground)] == 0) std::this_thread::yield();
  memory.stop();
  waiter.join();
  EXPECT_FALSE(got.has_value());
  EXPECT_FALSE(memory.try_acquire(WorkClass::control, hosted, 1));
}

TEST(RetainedMemory, ARequestLargerThanItsClassCanEverHaveIsRefusedAtOnce) {
  auto memory = ledger();
  EXPECT_FALSE(memory.acquire(WorkClass::foreground, hosted, 901, {}));
}

// Discipline 5's guard on the parts: over the limit, every part and every
// reserve shrinks by the same fraction, never below 1 MiB, and the node is
// told once what changed.
TEST(MemoryLimit, UnderTheLimitNothingChanges) {
  MemoryPlan plan{{{"a", 100ull << 20}, {"b", 100ull << 20}}, {{"control", 10ull << 20}}};
  EXPECT_FALSE(fit_to_limit(300ull << 20, plan).has_value());
  EXPECT_EQ(plan.parts[0].bytes, 100ull << 20);
  EXPECT_FALSE(fit_to_limit(0, plan).has_value());
}

TEST(MemoryLimit, OverTheLimitEveryPartAndReserveShrinksByTheSameFraction) {
  MemoryPlan plan{
      {{"dht.metadata_materialization_cache_bytes", 128ull << 20}, {"runtime.retained_memory_bytes", 768ull << 20}, {"storage.ledger_cache", 128ull << 20}},
      {{"control", 64ull << 20}, {"foreground", 192ull << 20}, {"loader", 64ull << 20}, {"reassembly", 32ull << 20}}};
  const auto warning = fit_to_limit(512ull << 20, plan);
  ASSERT_TRUE(warning.has_value());
  EXPECT_EQ(plan.parts[0].bytes, 64ull << 20);
  EXPECT_EQ(plan.parts[1].bytes, 384ull << 20);
  EXPECT_EQ(plan.parts[2].bytes, 64ull << 20);
  EXPECT_EQ(plan.reserves[1].bytes, 96ull << 20);
  EXPECT_EQ(plan.reserves[3].bytes, 16ull << 20);
  EXPECT_EQ(warning->rfind("MEMORY OVER LIMIT: the configured parts total 1024M, over runtime.memory_bytes 512M", 0), 0u);
  EXPECT_NE(warning->find("runtime.retained_memory_bytes 768M to 384M"), std::string::npos);
  EXPECT_NE(warning->find("(in-flight reserves control 64M to 32M"), std::string::npos);
}

TEST(MemoryLimit, NoPartShrinksBelowOneMebibyte) {
  MemoryPlan plan{{{"a", 2ull << 20}, {"b", 2000ull << 20}}, {}};
  ASSERT_TRUE(fit_to_limit(10ull << 20, plan).has_value());
  EXPECT_EQ(plan.parts[0].bytes, 1ull << 20);
}

}  // namespace
}  // namespace dannan::sched
