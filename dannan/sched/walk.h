//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "dannan/sched/work.h"

// One cursor, one budget, one page: every bounded walk is
// (state, cursor, budget) -> (page, next cursor). Discipline 4: long work is
// paged, and a page is a function of the budget, never of the state's size.
namespace dannan::sched {

// A position the caller owns: the last key handed out, or none for the start.
template <class Key>
struct Cursor {
  std::optional<Key> after;
  bool operator==(const Cursor&) const = default;
};

enum class Stop : uint8_t {
  end,        // the walk reached the end; the pass is complete
  budget,     // an operation, byte or item bound was spent
  deadline,   // the budget's or the context's deadline passed
  cancelled,  // the context was cancelled
  yield,      // the yield source asked for the turn back
};

template <class Item, class Key>
struct Page {
  std::vector<Item> items;
  Cursor<Key> next;
  Stop stopped = Stop::end;
  bool complete() const noexcept { return stopped == Stop::end; }
};

// Decides when background work gives way (a weighted share, a higher class
// active). Injected: its answers depend on node-level state.
class YieldSource {
 public:
  virtual ~YieldSource() = default;
  virtual bool should_yield() const = 0;
};

// What a walk may spend, and who is spending it. An absent bound never stops
// a walk. The yield source must outlive the budget. Single owner.
class Budget {
 public:
  explicit Budget(WorkContext context = WorkContext{}) noexcept : _context(context) {}

  Budget& operations(size_t limit) noexcept {
    _operations_left = limit;
    return *this;
  }
  Budget& bytes(uint64_t limit) noexcept {
    _bytes_left = limit;
    return *this;
  }
  Budget& deadline(TimePoint at) noexcept {
    _deadline = at;
    return *this;
  }
  Budget& yield_to(const YieldSource* source) noexcept {
    _yield = source;
    return *this;
  }

  const WorkContext& context() const noexcept { return _context; }

  // Why the walk must stop before its next step at `now`, if it must:
  // cancellation, then either deadline, then the yield source. The take_*
  // calls check the bounds.
  std::optional<Stop> must_stop(TimePoint now) const {
    if (_context.cancelled()) return Stop::cancelled;
    if ((_deadline && now >= *_deadline) || _context.expired(now)) return Stop::deadline;
    if (_yield && _yield->should_yield()) return Stop::yield;
    return std::nullopt;
  }
  // Spends one operation; false, spending nothing, when none is left.
  bool take_operation() noexcept {
    if (!_operations_left) return true;
    if (*_operations_left == 0) return false;
    --*_operations_left;
    return true;
  }
  // Spends `n` bytes; false, spending nothing, when fewer than `n` are left.
  bool take_bytes(uint64_t n) noexcept {
    if (!_bytes_left) return true;
    if (*_bytes_left < n) return false;
    *_bytes_left -= n;
    return true;
  }
  std::optional<size_t> operations_left() const noexcept { return _operations_left; }
  std::optional<uint64_t> bytes_left() const noexcept { return _bytes_left; }

 private:
  WorkContext _context;
  std::optional<size_t> _operations_left;
  std::optional<uint64_t> _bytes_left;
  std::optional<TimePoint> _deadline;
  const YieldSource* _yield = nullptr;
};

}  // namespace dannan::sched
