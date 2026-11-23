//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// The node's ledger of retained memory, by work class, with each class's reserve.

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/sched/class_waits.h"
#include "dannan/sched/work.h"

namespace dannan::sched {

/// Who holds retained memory, for the per-owner figures in Status. Dannan names its own owners below
/// 256; a hosted system numbers its own from `hosted_first` up, so the set of owners is open and a
/// figure is never Dannan's guess at a hosted system's shape.
enum class MemoryOwner : uint16_t {
  /// Inbound frames being reassembled, and requests until their handler returns.
  rpc_frame = 1,
  /// Large buffers the node's BufferPool holds for reuse.
  transfer_buffers = 2,
  /// What a deferring handler holds for its answer (net::Answer::hold(), 0.12.0).
  deferred_reply = 3,
  /// The first number a hosted system's own owners take.
  hosted_first = 256,
};

/// The ledger as it stands, for Status.
struct RetainedMemoryStats {
  /// The total budget.
  uint64_t capacity_bytes = 0;
  /// Headroom only control may use.
  uint64_t control_reserve_bytes = 0;
  /// Headroom kept for foreground.
  uint64_t foreground_reserve_bytes = 0;
  /// Headroom kept for loader.
  uint64_t loader_reserve_bytes = 0;
  /// Inbound reassembly's own reserve.
  uint64_t reassembly_reserve_bytes = 0;
  /// Held now.
  uint64_t used_bytes = 0;
  /// The most held at once since the ledger was made.
  uint64_t peak_used_bytes = 0;
  /// Held now by reclaimable leases, which shed when a higher class would wait.
  uint64_t reclaimable_bytes = 0;
  /// Held now, by owner.
  std::map<MemoryOwner, uint64_t> owner_bytes;
  /// Leases granted, by WorkClass.
  std::array<uint64_t, 4> admissions{};
  /// Waits begun, by WorkClass.
  std::array<uint64_t, 4> waits{};
  /// Times a reclaimable holder was asked to shed.
  uint64_t shed_requests = 0;
  /// Waits that ended cancelled.
  uint64_t cancelled_waits = 0;
  /// Bytes restored at recovery (restore()).
  uint64_t restored_bytes = 0;
};

/// The node's ledger of heap retained across an asynchronous boundary (a queued frame, a pending
/// publication, a cached page), one per node, handed to whatever retains; Context::budget() is a
/// hosted system's view of it.
///
/// Reserves are headroom inside one total, by class, so a lower class can never starve a higher one
/// (the four laws):
///
/// | Class | May use |
/// |---|---|
/// | control | the whole total (law 1) |
/// | foreground | everything but control's reserve (law 2) |
/// | loader | also leaves foreground's reserve; its durable bytes stand aside while control or foreground waits (law 3) |
/// | speculative | also leaves loader's reserve, and waits behind a waiting loader (law 4) |
///
/// Inbound reassembly (MemoryOwner::rpc_frame) has a small reserve of its own ahead of the lower
/// gates, and is exempt from the durable-lower budget: publication holds its bytes until a peer's
/// confirmation is reassembled, so charging reassembly to the same budget would deadlock.
/// Reclaimable memory may borrow idle headroom only with a shed callback, which a higher class that
/// would otherwise wait calls before waiting. Thread-safe.
class RetainedMemoryLedger {
  /// @cond internal
  struct Anchor;
  /// @endcond

 public:
  /// Held bytes, released when the lease goes. Move-only. A lease may outlive its ledger (a call's
  /// completion still on a shared io context after its node is destroyed): its release then does
  /// nothing.
  class Lease {
   public:
    /// Holds nothing.
    Lease() = default;
    /// Not copyable: one lease, one release.
    Lease(const Lease&) = delete;
    /// Not copyable.
    Lease& operator=(const Lease&) = delete;
    /// Moves the bytes held; the lease moved from holds nothing.
    Lease(Lease&& other) noexcept;
    /// Releases what this holds, then takes the other's.
    Lease& operator=(Lease&& other) noexcept;
    /// Releases what it holds.
    ~Lease() { reset(); }
    /// Releases what it holds now.
    void reset();
    /// The bytes held.
    uint64_t bytes() const noexcept { return _bytes; }
    /// Whether it holds anything.
    explicit operator bool() const noexcept { return _anchor != nullptr; }

   private:
    friend class RetainedMemoryLedger;
    Lease(std::shared_ptr<Anchor> anchor, uint64_t id, uint64_t bytes) : _anchor(std::move(anchor)), _id(id), _bytes(bytes) {}
    std::shared_ptr<Anchor> _anchor;
    uint64_t _id = 0;
    uint64_t _bytes = 0;
  };

  /// @param capacity_bytes the total.
  /// @throws std::invalid_argument unless every reserve is nonzero and they fit inside the total:
  ///   control, then foreground, then loader, then the reassembly reserve.
  RetainedMemoryLedger(uint64_t capacity_bytes, uint64_t control_reserve_bytes, uint64_t foreground_reserve_bytes, uint64_t loader_reserve_bytes,
                       uint64_t reassembly_reserve_bytes);
  /// Leases still held go quiet: their release does nothing. Waits for a release in flight on
  /// another thread.
  ~RetainedMemoryLedger();
  /// Not copyable.
  RetainedMemoryLedger(const RetainedMemoryLedger&) = delete;
  /// Not copyable.
  RetainedMemoryLedger& operator=(const RetainedMemoryLedger&) = delete;
  /// Not movable: leases point at it.
  RetainedMemoryLedger(RetainedMemoryLedger&&) = delete;

  /// Waits until `bytes` may be held. Waits on locks and on other holders.
  /// @param deadline on the steady clock; a default TimePoint: none.
  /// @param cancelled when set and the ledger woken (wake()), the wait ends empty.
  /// @param reclaimable the bytes may be shed: they borrow idle headroom, and `shed` is called when a
  ///   higher class would otherwise wait.
  /// @return the lease; empty when the deadline passes, it is cancelled, or the ledger stops, and at
  ///   once when the class could never hold that much.
  std::optional<Lease> acquire(WorkClass, MemoryOwner, uint64_t bytes, TimePoint deadline, std::atomic_bool* cancelled = nullptr, bool reclaimable = false,
                               std::function<void()> shed = {});
  /// Never waits: a lease if `bytes` may be held now, else empty.
  std::optional<Lease> try_acquire(WorkClass, MemoryOwner, uint64_t bytes, bool reclaimable = false, std::function<void()> shed = {});
  /// Ownership of work already acknowledged, reconstructed at recovery. Never refused; an overcommit
  /// blocks new admissions until it drains.
  Lease restore(WorkClass, MemoryOwner, uint64_t bytes);
  /// Wakes every waiter to check its cancellation flag again.
  void wake();
  /// Ends every wait empty and refuses every later admission.
  void stop();
  /// The ledger as it stands.
  RetainedMemoryStats stats() const;

  /// @cond internal
 private:
  struct Allocation {
    WorkClass work = WorkClass::control;
    MemoryOwner owner = MemoryOwner::rpc_frame;
    uint64_t bytes = 0;
    bool reclaimable = false;
    bool shed_requested = false;
    std::function<void()> shed;
  };

  // What every lease holds in place of the ledger: the ledger while it lives, nothing
  // after. A release holds it shared across the release; the ledger's end takes it
  // exclusively. Lock order: the anchor's, then the ledger's _mutex.
  struct Anchor {
    explicit Anchor(RetainedMemoryLedger* owner) : ledger(owner) {}
    contract::SharedMutex mutex;
    RetainedMemoryLedger* ledger DANNAN_GUARDED_BY(mutex);
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
  ClassWaits _classes DANNAN_GUARDED_BY(_mutex);
  uint64_t _shed_requests DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _restored DANNAN_GUARDED_BY(_mutex) = 0;
  bool _stopping DANNAN_GUARDED_BY(_mutex) = false;
  const std::shared_ptr<Anchor> _anchor = std::make_shared<Anchor>(this);
  /// @endcond
};

}  // namespace dannan::sched
