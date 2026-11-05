//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/sched/retained_memory.h"

#include <algorithm>
#include <stdexcept>

namespace dannan::sched {
namespace {

bool lower(WorkClass work) { return work == WorkClass::loader || work == WorkClass::speculative; }

// Higher is more urgent: control 3, foreground 2, loader 1, speculative 0.
int priority(WorkClass work) { return 3 - static_cast<int>(work); }

// Nothing is free: a zero-byte request still holds one byte.
uint64_t charge(uint64_t bytes) { return std::max<uint64_t>(1, bytes); }

}  // namespace

RetainedMemoryLedger::Lease::Lease(Lease&& other) noexcept : _owner(other._owner), _id(other._id), _bytes(other._bytes) {
  other._owner = nullptr;
  other._id = 0;
  other._bytes = 0;
}

RetainedMemoryLedger::Lease& RetainedMemoryLedger::Lease::operator=(Lease&& other) noexcept {
  if (this != &other) {
    reset();
    _owner = other._owner;
    _id = other._id;
    _bytes = other._bytes;
    other._owner = nullptr;
    other._id = 0;
    other._bytes = 0;
  }
  return *this;
}

void RetainedMemoryLedger::Lease::reset() {
  if (!_owner) return;
  _owner->release(_id);
  _owner = nullptr;
  _id = 0;
  _bytes = 0;
}

RetainedMemoryLedger::RetainedMemoryLedger(uint64_t capacity_bytes, uint64_t control_reserve_bytes, uint64_t foreground_reserve_bytes,
                                           uint64_t loader_reserve_bytes, uint64_t reassembly_reserve_bytes)
    : _capacity(capacity_bytes),
      _control_reserve(control_reserve_bytes),
      _foreground_reserve(foreground_reserve_bytes),
      _loader_reserve(loader_reserve_bytes),
      _reassembly_reserve(reassembly_reserve_bytes) {
  if (!_capacity || !_control_reserve || !_foreground_reserve || !_loader_reserve || !_reassembly_reserve || _control_reserve > _capacity ||
      _foreground_reserve > _capacity - _control_reserve || _loader_reserve > _capacity - _control_reserve - _foreground_reserve ||
      _reassembly_reserve > _capacity - _control_reserve - _foreground_reserve - _loader_reserve)
    throw std::invalid_argument("retained-memory reserves exceed capacity");
}

bool RetainedMemoryLedger::available_locked(WorkClass work, MemoryOwner owner, uint64_t bytes, bool reclaimable) const {
  if (bytes > _capacity || _used > _capacity - bytes) return false;
  if (work == WorkClass::control) return true;
  const auto non_control = _capacity - _control_reserve;
  if (bytes > non_control || _used > non_control - bytes) return false;
  if (work == WorkClass::foreground) return true;
  if (_classes.waiting(WorkClass::control) || _classes.waiting(WorkClass::foreground)) return false;
  if (owner == MemoryOwner::rpc_frame) {
    const auto found = _owner_bytes.find(MemoryOwner::rpc_frame);
    const uint64_t held = found == _owner_bytes.end() ? 0 : found->second;
    if (held + bytes <= _reassembly_reserve) return true;
  }
  if (work == WorkClass::speculative && _classes.waiting(WorkClass::loader)) return false;
  if (reclaimable) return true;
  // Reassembly's exemption from the durable-lower budget is bounded by the
  // transport's own reassembly limits; the class reserves above still apply.
  if (owner == MemoryOwner::rpc_frame) return true;
  const auto durable_lower = non_control - _foreground_reserve;
  if (bytes > durable_lower || _lower_durable > durable_lower - bytes) return false;
  if (work == WorkClass::loader) return true;
  const auto speculative = durable_lower - _loader_reserve;
  return bytes <= speculative && _speculative_durable <= speculative - bytes;
}

std::vector<std::function<void()>> RetainedMemoryLedger::request_shedding_locked(WorkClass incoming) {
  std::vector<std::function<void()>> callbacks;
  for (auto& [_, allocation] : _allocations) {
    if (!allocation.reclaimable || allocation.shed_requested || priority(allocation.work) >= priority(incoming)) continue;
    allocation.shed_requested = true;
    ++_shed_requests;
    callbacks.push_back(allocation.shed);
  }
  return callbacks;
}

RetainedMemoryLedger::Lease RetainedMemoryLedger::admit_locked(WorkClass work, MemoryOwner owner, uint64_t bytes, bool reclaimable,
                                                               std::function<void()> shed) {
  const auto id = _next_id++;
  _allocations.emplace(id, Allocation{work, owner, bytes, reclaimable, false, std::move(shed)});
  _used += bytes;
  _owner_bytes[owner] += bytes;
  if (reclaimable) {
    _reclaimable += bytes;
  } else if (lower(work)) {
    _lower_durable += bytes;
    if (work == WorkClass::speculative) _speculative_durable += bytes;
  }
  _peak = std::max(_peak, _used);
  ++_admissions[index(work)];
  return Lease(*this, id, bytes);
}

std::optional<RetainedMemoryLedger::Lease> RetainedMemoryLedger::acquire(WorkClass work, MemoryOwner owner, uint64_t requested, TimePoint deadline,
                                                                         std::atomic_bool* cancelled, bool reclaimable, std::function<void()> shed) {
  if (reclaimable && !shed) throw std::invalid_argument("reclaimable retained memory requires a shed callback");
  const auto bytes = charge(requested);
  const auto ceiling = work == WorkClass::control ? _capacity : _capacity - _control_reserve;
  if (bytes > ceiling) return std::nullopt;
  contract::Lock lock(_mutex);
  ClassWaits::Bounds bounds;
  bounds.deadline = deadline;
  bounds.cancelled = cancelled;
  const auto outcome = _classes.wait(
      lock, _changed, work, bounds, [&]() DANNAN_NO_THREAD_SAFETY_ANALYSIS { return _stopping; },
      [&]() DANNAN_NO_THREAD_SAFETY_ANALYSIS { return available_locked(work, owner, bytes, reclaimable); }, [] {},
      [&](contract::Lock& held) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
        auto callbacks = request_shedding_locked(work);
        if (callbacks.empty()) return false;
        // Shedding runs a holder's code: never under the ledger's lock.
        held.unlock();
        for (auto& callback : callbacks) {
          try {
            callback();
          } catch (...) {
            // Best effort: the still-live lease stays charged and the waiter
            // stays bounded by its deadline.
          }
        }
        held.lock();
        return true;
      });
  if (outcome != ClassWaits::Outcome::admitted) return std::nullopt;
  return admit_locked(work, owner, bytes, reclaimable, std::move(shed));
}

std::optional<RetainedMemoryLedger::Lease> RetainedMemoryLedger::try_acquire(WorkClass work, MemoryOwner owner, uint64_t requested, bool reclaimable,
                                                                             std::function<void()> shed) {
  if (reclaimable && !shed) throw std::invalid_argument("reclaimable retained memory requires a shed callback");
  const auto bytes = charge(requested);
  contract::Lock lock(_mutex);
  if (_stopping || !available_locked(work, owner, bytes, reclaimable)) return std::nullopt;
  return admit_locked(work, owner, bytes, reclaimable, std::move(shed));
}

RetainedMemoryLedger::Lease RetainedMemoryLedger::restore(WorkClass work, MemoryOwner owner, uint64_t requested) {
  const auto bytes = charge(requested);
  contract::Lock lock(_mutex);
  _restored += bytes;
  auto lease = admit_locked(work, owner, bytes, false, {});
  --_admissions[index(work)];  // a restore is not an admission
  return lease;
}

void RetainedMemoryLedger::release(uint64_t id) {
  contract::Lock lock(_mutex);
  const auto found = _allocations.find(id);
  if (found == _allocations.end()) return;
  const auto allocation = std::move(found->second);
  _allocations.erase(found);
  _used -= allocation.bytes;
  if ((_owner_bytes[allocation.owner] -= allocation.bytes) == 0) _owner_bytes.erase(allocation.owner);
  if (allocation.reclaimable) {
    _reclaimable -= allocation.bytes;
  } else if (lower(allocation.work)) {
    _lower_durable -= allocation.bytes;
    if (allocation.work == WorkClass::speculative) _speculative_durable -= allocation.bytes;
  }
  _classes.released();
  _changed.notify_all();
}

void RetainedMemoryLedger::wake() {
  contract::Lock lock(_mutex);
  _changed.notify_all();
}

void RetainedMemoryLedger::stop() {
  contract::Lock lock(_mutex);
  _stopping = true;
  _changed.notify_all();
}

RetainedMemoryStats RetainedMemoryLedger::stats() const {
  contract::Lock lock(_mutex);
  RetainedMemoryStats out;
  out.capacity_bytes = _capacity;
  out.control_reserve_bytes = _control_reserve;
  out.foreground_reserve_bytes = _foreground_reserve;
  out.loader_reserve_bytes = _loader_reserve;
  out.reassembly_reserve_bytes = _reassembly_reserve;
  out.used_bytes = _used;
  out.peak_used_bytes = _peak;
  out.reclaimable_bytes = _reclaimable;
  out.owner_bytes = _owner_bytes;
  out.admissions = _admissions;
  out.waits = _classes.all_waits();
  out.shed_requests = _shed_requests;
  out.cancelled_waits = _classes.gave_up();
  out.restored_bytes = _restored;
  return out;
}

}  // namespace dannan::sched
