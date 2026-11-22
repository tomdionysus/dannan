//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/buffer_pool.h"

#include <utility>

namespace dannan::sched {

BufferPool::BufferPool(size_t bound_bytes, size_t floor_bytes, RetainedMemoryLedger* ledger) : _bound(bound_bytes), _floor(floor_bytes), _ledger(ledger) {}

Bytes BufferPool::take(size_t n) {
  Bytes out;
  {
    // What is let go here (a lease) goes after the lock.
    std::optional<Held> found;
    {
      contract::Lock lock(_mutex);
      ++_stats.taken;
      if (n >= _floor) {
        const auto fit = _held.lower_bound(n);
        if (fit != _held.end() && fit->first / 2 <= n) {
          found = std::move(fit->second);
          _stats.held_bytes -= fit->first;
          --_stats.held_buffers;
          ++_stats.reused;
          _held.erase(fit);
        }
      }
    }
    if (found) {
      out = std::move(found->buffer);
      out.clear();
      return out;
    }
  }
  out.reserve(n);
  return out;
}

void BufferPool::give(Bytes buffer) {
  const auto capacity = buffer.capacity();
  std::optional<RetainedMemoryLedger::Lease> lease;
  contract::Lock lock(_mutex);
  const bool fits = !_detached && capacity >= _floor && _stats.held_bytes + capacity <= _bound;
  if (fits && _ledger) {
    lease = _ledger->try_acquire(WorkClass::speculative, MemoryOwner::transfer_buffers, capacity);
  }
  if (!fits || (_ledger && !lease)) {
    ++_stats.freed;
    return;  // freed as `buffer` goes, after the lock
  }
  _held.emplace(capacity, Held{std::move(buffer), lease ? std::move(*lease) : RetainedMemoryLedger::Lease{}});
  _stats.held_bytes += capacity;
  ++_stats.held_buffers;
  ++_stats.kept;
}

void BufferPool::detach() {
  std::multimap<size_t, Held> dropped;
  {
    contract::Lock lock(_mutex);
    _detached = true;
    _ledger = nullptr;
    dropped.swap(_held);
    _stats.held_bytes = 0;
    _stats.held_buffers = 0;
  }
}

BufferPoolStats BufferPool::stats() const {
  contract::Lock lock(_mutex);
  return _stats;
}

}  // namespace dannan::sched
