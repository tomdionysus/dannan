//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/sched/work.h"

namespace dannan::sched {

// Who holds retained memory, for the per-owner figures in Status. Dannan
// names its own owners below 256; a hosted system numbers its own from
// `hosted_first` up, so the set of owners is open and a figure is never
// Dannan's guess at a hosted system's shape.
enum class MemoryOwner : uint16_t {
  rpc_frame = 1,  // inbound frames being reassembled, and requests until their handler returns
  hosted_first = 256,
};

struct RetainedMemoryStats {
  uint64_t capacity_bytes = 0;
  uint64_t control_reserve_bytes = 0;
  uint64_t foreground_reserve_bytes = 0;
  uint64_t loader_reserve_bytes = 0;
  uint64_t reassembly_reserve_bytes = 0;
  uint64_t used_bytes = 0;
  uint64_t peak_used_bytes = 0;
  uint64_t reclaimable_bytes = 0;
  std::map<MemoryOwner, uint64_t> owner_bytes;
  // Indexed by WorkClass.
  std::array<uint64_t, 4> admissions{};
  std::array<uint64_t, 4> waits{};
  uint64_t shed_requests = 0;
  uint64_t cancelled_waits = 0;
  uint64_t restored_bytes = 0;
};

// The node's ledger of heap retained across an asynchronous boundary (a
// queued frame, a pending publication, a cached page), one per node, handed
// to whatever retains. Reserves are headroom inside one total, by class:
//
//   control      may use the whole total (law 1);
//   foreground   everything but control's reserve (law 2);
//   loader       also leaves foreground's reserve, and its durable bytes
//                stand aside while control or foreground waits (law 3);
//   speculative  also leaves loader's reserve, and waits behind a waiting
//                loader (law 4).
//
// Inbound reassembly (MemoryOwner::rpc_frame) has a small reserve of its own
// ahead of the lower gates, and is exempt from the durable-lower budget:
// publication holds its bytes until a peer's confirmation is reassembled, so
// charging reassembly to the same budget deadlocks. Reclaimable memory may
// borrow idle headroom only with a shed callback, which a higher class that
// would otherwise wait calls before waiting.
class RetainedMemoryLedger {
 public:
  // Held bytes, released when the lease goes. Move-only.
  class Lease {
   public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;
    ~Lease() { reset(); }
    void reset();
    uint64_t bytes() const noexcept { return _bytes; }
    explicit operator bool() const noexcept { return _owner != nullptr; }

   private:
    friend class RetainedMemoryLedger;
    Lease(RetainedMemoryLedger& owner, uint64_t id, uint64_t bytes) : _owner(&owner), _id(id), _bytes(bytes) {}
    RetainedMemoryLedger* _owner = nullptr;
    uint64_t _id = 0;
    uint64_t _bytes = 0;
  };

  // Throws std::invalid_argument unless every reserve is nonzero and they
  // fit inside the total: control, then foreground, then loader, then the
  // reassembly reserve.
  RetainedMemoryLedger(uint64_t capacity_bytes, uint64_t control_reserve_bytes, uint64_t foreground_reserve_bytes, uint64_t loader_reserve_bytes,
                       uint64_t reassembly_reserve_bytes);
  RetainedMemoryLedger(const RetainedMemoryLedger&) = delete;
  RetainedMemoryLedger& operator=(const RetainedMemoryLedger&) = delete;
  RetainedMemoryLedger(RetainedMemoryLedger&&) = delete;

  // Waits until `bytes` may be held, the deadline passes (on the steady
  // clock; none when default), `cancelled` is set and the ledger woken, or
  // the ledger stops; empty in the last three cases, and at once when the
  // class could never hold that much. Waits on locks and on other holders;
  // thread-safe.
  std::optional<Lease> acquire(WorkClass, MemoryOwner, uint64_t bytes, TimePoint deadline, std::atomic_bool* cancelled = nullptr, bool reclaimable = false,
                               std::function<void()> shed = {});
  // Never waits: a lease if `bytes` may be held now, else empty.
  std::optional<Lease> try_acquire(WorkClass, MemoryOwner, uint64_t bytes, bool reclaimable = false, std::function<void()> shed = {});
  // Ownership of work already acknowledged, reconstructed at recovery. Never
  // refused; an overcommit blocks new admissions until it drains.
  Lease restore(WorkClass, MemoryOwner, uint64_t bytes);
  // Wakes every waiter to re-check its cancellation flag.
  void wake();
  // Ends every wait empty and refuses every later admission.
  void stop();
  RetainedMemoryStats stats() const;

 private:
  struct Allocation {
    WorkClass work = WorkClass::control;
    MemoryOwner owner = MemoryOwner::rpc_frame;
    uint64_t bytes = 0;
    bool reclaimable = false;
    bool shed_requested = false;
    std::function<void()> shed;
  };

  static constexpr size_t index(WorkClass work) noexcept { return static_cast<size_t>(work); }
  bool available_locked(WorkClass, MemoryOwner, uint64_t bytes, bool reclaimable) const DANNAN_REQUIRES(_mutex);
  std::vector<std::function<void()>> request_shedding_locked(WorkClass incoming) DANNAN_REQUIRES(_mutex);
  Lease admit_locked(WorkClass, MemoryOwner, uint64_t bytes, bool reclaimable, std::function<void()> shed) DANNAN_REQUIRES(_mutex);
  void release(uint64_t id);

  const uint64_t _capacity;
  const uint64_t _control_reserve;
  const uint64_t _foreground_reserve;
  const uint64_t _loader_reserve;
  const uint64_t _reassembly_reserve;
  mutable contract::Mutex _mutex;
  std::condition_variable _changed;
  std::map<uint64_t, Allocation> _allocations DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_id DANNAN_GUARDED_BY(_mutex) = 1;
  uint64_t _used DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _reclaimable DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _lower_durable DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _speculative_durable DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _peak DANNAN_GUARDED_BY(_mutex) = 0;
  std::map<MemoryOwner, uint64_t> _owner_bytes DANNAN_GUARDED_BY(_mutex);
  std::array<uint64_t, 4> _admissions DANNAN_GUARDED_BY(_mutex){};
  std::array<uint64_t, 4> _waits DANNAN_GUARDED_BY(_mutex){};
  std::array<uint64_t, 4> _waiters DANNAN_GUARDED_BY(_mutex){};
  uint64_t _shed_requests DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _cancelled_waits DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _restored DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopping DANNAN_GUARDED_BY(_mutex) = false;
};

}  // namespace dannan::sched
