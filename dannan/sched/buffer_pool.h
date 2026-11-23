//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/sched/retained_memory.h"

namespace dannan::sched {

using Bytes = std::vector<uint8_t>;

struct BufferPoolStats {
  uint64_t held_bytes = 0;
  uint64_t held_buffers = 0;
  uint64_t taken = 0;
  // Takes answered from what was held.
  uint64_t reused = 0;
  // Gives kept, and gives freed, and why: under the floor, past the bound, refused by the ledger,
  // or given after the pool was detached.
  uint64_t kept = 0;
  uint64_t freed = 0;
  uint64_t freed_small = 0;
  uint64_t freed_bound = 0;
  uint64_t freed_ledger = 0;
  uint64_t freed_detached = 0;
};

// The node's pool of large transfer buffers (0.11.0; Dagda's case): a block of an extent's size
// is reused rather than freed and allocated again. glibc's mmap threshold slides up to the
// largest block freed, so after the first 16 MiB free every later one comes from a per-thread
// arena, which keeps its high-water mark for good; a pool keeps the blocks in use instead.
//
// Bounded in bytes held; what it holds is charged to the retained-memory ledger as
// MemoryOwner::transfer_buffers, and a buffer the ledger refuses is freed. Buffers under the
// floor are never kept. Thread-safe; waits on nothing but its own lock. Held by shared_ptr, so a
// holder may give a buffer back after the node has gone: detach() (the node's, as it goes) lets
// go of the ledger, and from then on what is given is freed.
class BufferPool {
 public:
  BufferPool(size_t bound_bytes, size_t floor_bytes, RetainedMemoryLedger* ledger = nullptr);
  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // A buffer of size 0 and capacity at least `n`: the smallest held one that fits and is at
  // most twice `n`, else a new one of exactly `n`.
  Bytes take(size_t n);
  // Kept for a later take, or freed; the caller is done with it either way.
  void give(Bytes buffer);
  // Frees all it holds, lets go of the ledger, and keeps nothing more.
  void detach();

  BufferPoolStats stats() const;

 private:
  struct Held {
    Bytes buffer;
    RetainedMemoryLedger::Lease lease;
  };

  const size_t _bound;
  const size_t _floor;
  mutable contract::Mutex _mutex;
  RetainedMemoryLedger* _ledger DANNAN_GUARDED_BY(_mutex);
  bool _detached DANNAN_GUARDED_BY(_mutex) = false;
  // By capacity.
  std::multimap<size_t, Held> _held DANNAN_GUARDED_BY(_mutex);
  BufferPoolStats _stats DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::sched
