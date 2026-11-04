//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"

namespace dannan::status {

// What has happened on this node, one monotonic count per kind. Producers
// only notify; consumers keep the counts they last saw and react to the
// difference, so nothing calls out of a producer's thread and no consumer is
// installed or removed. Dannan's own kinds are topology (the roster
// changed: a node joined or left, or its place, capabilities, path class,
// incarnation or adverts changed) and membership (the same less adverts,
// for a consumer that re-asks peers and must not on every advert a busy
// cluster changes). A hosted system names its own (storage, metadata,
// claims) before it starts. Thread-safe.
class NodeEvents {
 public:
  using Kind = size_t;
  static constexpr Kind topology = 0;
  static constexpr Kind membership = 1;
  // Dannan's kinds; a hosted system's are numbered after them.
  static constexpr size_t dannan_kinds = 2;
  static constexpr size_t max_kinds = 32;

  // The kind with this name, made on first use. Throws std::length_error
  // past max_kinds. Takes a lock: call it at start, not per event.
  Kind kind(const std::string& name);

  void notify(Kind kind);
  uint64_t count(Kind kind) const noexcept { return _counts[kind].load(std::memory_order_acquire); }
  // Moves whenever any count does.
  uint64_t total() const noexcept;
  // Waits until total() differs from `seen`, or `timeout`; whether it did.
  bool wait_for_change(uint64_t seen, std::chrono::milliseconds timeout);

 private:
  std::array<std::atomic<uint64_t>, max_kinds> _counts{};
  mutable contract::Mutex _names_mutex;
  std::vector<std::string> _names DANNAN_GUARDED_BY(_names_mutex){"topology", "membership"};
  // Guards nothing: orders a notify against a waiter's check.
  contract::Mutex _wait_mutex;
  std::condition_variable _changed;
};

}  // namespace dannan::status
