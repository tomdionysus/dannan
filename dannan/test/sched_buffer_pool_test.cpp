//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The node's pool of large transfer buffers (0.11.0; Dagda's numbers, the maintainer's yes): a
// block of an extent's size is reused rather than freed and allocated again, since glibc's
// sliding mmap threshold keeps each freed 16 MiB block in a per-thread arena for good. Bounded
// in bytes held, charged to the retained-memory ledger, safe from any thread and after its node
// has gone.

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "dannan/sched/buffer_pool.h"

namespace dannan::sched {
namespace {

constexpr size_t mib = 1024 * 1024;

Bytes filled(size_t n) {
  Bytes out(n, 0x5a);
  return out;
}

TEST(BufferPool, ABufferGivenBackIsTakenAgain) {
  BufferPool pool(64 * mib, mib);
  auto first = pool.take(16 * mib);
  EXPECT_EQ(first.size(), 0u);
  EXPECT_GE(first.capacity(), 16 * mib);
  first.resize(16 * mib);
  const auto* block = first.data();
  pool.give(std::move(first));
  EXPECT_EQ(pool.stats().held_bytes, 16 * mib);
  auto again = pool.take(16 * mib - 36);
  EXPECT_EQ(again.data(), block);
  EXPECT_EQ(again.size(), 0u);
  EXPECT_EQ(pool.stats().held_bytes, 0u);
  EXPECT_EQ(pool.stats().reused, 1u);
}

// Never much larger than asked for: a 1 MiB request does not take a 16 MiB block the next
// extent would want (Dagda).
TEST(BufferPool, ABufferMoreThanTwiceTheRequestIsLeftForALargerOne) {
  BufferPool pool(64 * mib, mib);
  pool.give(filled(16 * mib));
  auto small = pool.take(mib);
  EXPECT_EQ(small.capacity(), mib);
  EXPECT_EQ(pool.stats().held_bytes, 16 * mib);
  auto large = pool.take(9 * mib);
  EXPECT_GE(large.capacity(), 16 * mib);
  EXPECT_EQ(pool.stats().held_bytes, 0u);
}

TEST(BufferPool, ItTakesTheSmallestThatFits) {
  BufferPool pool(64 * mib, mib);
  pool.give(filled(16 * mib));
  pool.give(filled(4 * mib));
  pool.give(filled(8 * mib));
  EXPECT_EQ(pool.take(3 * mib).capacity(), 4 * mib);
  EXPECT_EQ(pool.take(5 * mib).capacity(), 8 * mib);
}

TEST(BufferPool, SmallBuffersAreNotKept) {
  BufferPool pool(64 * mib, mib);
  pool.give(filled(mib - 1));
  EXPECT_EQ(pool.stats().held_bytes, 0u);
  EXPECT_EQ(pool.take(mib - 1).capacity(), mib - 1);
}

// Why each give was freed is counted, so a gate can say what it sees (Dagda).
TEST(BufferPool, ItCountsWhyEachFreedBufferWasFreed) {
  RetainedMemoryLedger ledger(64 * mib, 8 * mib, 8 * mib, 8 * mib, 8 * mib);
  BufferPool pool(36 * mib, mib, &ledger);
  pool.give(filled(mib / 2));
  EXPECT_EQ(pool.stats().freed_small, 1u);
  pool.give(filled(16 * mib));
  pool.give(filled(16 * mib));
  pool.give(filled(8 * mib));
  EXPECT_EQ(pool.stats().freed_bound, 1u);
  EXPECT_EQ(pool.stats().freed_ledger, 0u);
  // 32 MiB of the ledger's 64 is held already: another 16 for speculative work is refused.
  BufferPool tight(256 * mib, mib, &ledger);
  tight.give(filled(16 * mib));
  EXPECT_EQ(tight.stats().freed_ledger, 1u);
  pool.detach();
  pool.give(filled(16 * mib));
  EXPECT_EQ(pool.stats().freed_detached, 1u);
  EXPECT_EQ(pool.stats().freed, pool.stats().freed_small + pool.stats().freed_bound + pool.stats().freed_ledger + pool.stats().freed_detached);
}

TEST(BufferPool, WhatItHoldsStaysWithinItsBound) {
  BufferPool pool(40 * mib, mib);
  pool.give(filled(16 * mib));
  pool.give(filled(16 * mib));
  pool.give(filled(16 * mib));
  EXPECT_EQ(pool.stats().held_bytes, 32 * mib);
  EXPECT_EQ(pool.stats().freed, 1u);
}

TEST(BufferPool, WhatItHoldsIsChargedToTheLedger) {
  RetainedMemoryLedger ledger(256 * mib, 16 * mib, 16 * mib, 16 * mib, 16 * mib);
  BufferPool pool(64 * mib, mib, &ledger);
  pool.give(filled(16 * mib));
  EXPECT_EQ(ledger.stats().owner_bytes[MemoryOwner::transfer_buffers], 16 * mib);
  (void)pool.take(16 * mib);
  EXPECT_EQ(ledger.stats().owner_bytes[MemoryOwner::transfer_buffers], 0u);
}

// The ledger full, a buffer is freed rather than kept uncharged.
TEST(BufferPool, ABufferTheLedgerRefusesIsFreed) {
  RetainedMemoryLedger ledger(64 * mib, 8 * mib, 8 * mib, 8 * mib, 8 * mib);
  BufferPool pool(256 * mib, mib, &ledger);
  pool.give(filled(16 * mib));
  pool.give(filled(16 * mib));
  pool.give(filled(16 * mib));
  EXPECT_LE(pool.stats().held_bytes, 64 * mib);
  EXPECT_GE(pool.stats().freed, 1u);
  EXPECT_EQ(ledger.stats().owner_bytes[MemoryOwner::transfer_buffers], pool.stats().held_bytes);
}

// A holder may give a buffer back after its node has gone: the pool, detached from the ledger,
// frees what it is given and holds nothing.
TEST(BufferPool, AfterItIsDetachedItFreesWhatItIsGiven) {
  auto ledger = std::make_unique<RetainedMemoryLedger>(256 * mib, 16 * mib, 16 * mib, 16 * mib, 16 * mib);
  auto pool = std::make_shared<BufferPool>(64 * mib, mib, ledger.get());
  pool->give(filled(16 * mib));
  pool->detach();
  ledger.reset();
  EXPECT_EQ(pool->stats().held_bytes, 0u);
  pool->give(filled(16 * mib));
  EXPECT_EQ(pool->stats().held_bytes, 0u);
  EXPECT_GE(pool->take(16 * mib).capacity(), 16 * mib);
}

TEST(BufferPool, ManyThreadsTakeAndGiveAtOnce) {
  RetainedMemoryLedger ledger(512 * mib, 16 * mib, 16 * mib, 16 * mib, 16 * mib);
  BufferPool pool(128 * mib, mib, &ledger);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
    threads.emplace_back([&pool, t] {
      for (int i = 0; i < 200; ++i) {
        auto buffer = pool.take(static_cast<size_t>(1 + (i + t) % 4) * mib);
        buffer.resize(buffer.capacity());
        pool.give(std::move(buffer));
      }
    });
  for (auto& thread : threads) thread.join();
  EXPECT_LE(pool.stats().held_bytes, 128 * mib);
  EXPECT_EQ(ledger.stats().owner_bytes[MemoryOwner::transfer_buffers], pool.stats().held_bytes);
}

}  // namespace
}  // namespace dannan::sched
