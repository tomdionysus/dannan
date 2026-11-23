//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// The node's pool of large transfer buffers.

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/sched/retained_memory.h"

namespace dannan::sched {

/// A buffer of bytes, as the pool hands them out.
using Bytes = std::vector<uint8_t>;

/// What a BufferPool has done since it was made, for Status.
struct BufferPoolStats {
  /// Bytes held for reuse now (their capacities).
  uint64_t held_bytes = 0;
  /// Buffers held for reuse now.
  uint64_t held_buffers = 0;
  /// Every take().
  uint64_t taken = 0;
  /// Takes answered from what was held.
  uint64_t reused = 0;
  /// Gives kept for reuse.
  uint64_t kept = 0;
  /// Gives freed, for one of the reasons below.
  uint64_t freed = 0;
  /// Freed being under the floor.
  uint64_t freed_small = 0;
  /// Freed because keeping them would pass the bound.
  uint64_t freed_bound = 0;
  /// Freed because the retained-memory ledger refused them.
  uint64_t freed_ledger = 0;
  /// Freed because the pool was detached (its node had gone).
  uint64_t freed_detached = 0;
};

/// The node's pool of large transfer buffers (0.11.0), as Context::buffers().
///
/// Why: a block of an extent's size (16 MiB) is reused rather than freed and allocated again.
/// glibc's mmap threshold slides up to the largest block freed, so after the first 16 MiB free
/// every later one comes from a per-thread arena, which keeps its high-water mark for good; a
/// pool keeps the blocks in use instead (Dagda's repair case: held memory fell from 400-620 MiB to
/// flat).
///
/// Bounded in bytes held; what it holds is charged to the retained-memory ledger as
/// MemoryOwner::transfer_buffers, and a buffer the ledger refuses is freed. Buffers under the floor
/// are never kept. A hosted system that keeps windows of large buffers of its own sizes them under
/// bound() (Dagda's repair window: the bound less four extents), so what it gives back is kept.
///
/// Ownership: held by shared_ptr, so a holder may give a buffer back after the node has gone;
/// detach() (the node's, as it goes) lets go of the ledger, and from then on what is given is freed.
/// Thread-safe; waits on nothing but its own lock.
///
/// @code
/// auto pool = context.buffers();
/// auto buffer = pool->take(extent_bytes);  // size 0, capacity at least extent_bytes
/// buffer.resize(extent_bytes);
/// // ... fill it, send it, and when done:
/// pool->give(std::move(buffer));
/// @endcode
class BufferPool {
 public:
  /// @param bound_bytes the most bytes (capacities) it holds for reuse.
  /// @param floor_bytes buffers smaller than this are never kept.
  /// @param ledger what holding is charged to; none: nothing is charged. It must outlive the pool
  ///   or be let go by detach() first.
  BufferPool(size_t bound_bytes, size_t floor_bytes, RetainedMemoryLedger* ledger = nullptr);
  /// Not copyable: one pool, one set of blocks.
  BufferPool(const BufferPool&) = delete;
  /// Not copyable.
  BufferPool& operator=(const BufferPool&) = delete;

  /// A buffer of size 0 and capacity at least `n`: the smallest held one that fits and is at most
  /// twice `n`, else a new one of exactly `n`.
  Bytes take(size_t n);
  /// Gives a buffer back: kept for a later take(), or freed (stats() says why). The caller is done
  /// with it either way.
  void give(Bytes buffer);
  /// Frees all it holds, lets go of the ledger, and keeps nothing more. The node's, as it goes.
  void detach();

  /// What it has done since it was made.
  BufferPoolStats stats() const;
  /// The most bytes it holds (0.11.1, for a host sizing its own windows to fit). Inline.
  size_t bound() const { return _bound; }
  /// The least size it keeps (0.11.1). Inline.
  size_t floor() const { return _floor; }

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
