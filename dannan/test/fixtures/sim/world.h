//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>

#include "dannan/contract/time_source.h"

// The deterministic simulation harness (TODO/2026-11-15-simulation-harness.md): a world
// on one thread, with no real time, sockets or disks; every choice drawn from one seed, so
// a seed reproduces a run exactly, on any compiler.
namespace dannan::test::sim {

// SplitMix64, with draws of the harness's own: the standard library's distributions
// differ between libc++ and libstdc++, so a seed found on a Pi would not replay on the Mac
// (Kvasir's review).
class Random {
 public:
  explicit Random(uint64_t seed) noexcept : _state(seed) {}
  uint64_t next() noexcept {
    uint64_t z = (_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  // In [0, bound); 0 for a bound of 0.
  uint64_t below(uint64_t bound) noexcept { return bound ? next() % bound : 0; }
  // True one time in `in`.
  bool one_in(uint64_t in) noexcept { return in && below(in) == 0; }

 private:
  uint64_t _state;
};

// The world: its time, its queue of events, and one io_context whose ready handlers run
// between events, one at a time. Single-threaded; nothing here is thread-safe.
class World {
 public:
  explicit World(uint64_t seed) : random(seed) {}
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  Random random;
  boost::asio::io_context io;

  // Milliseconds of simulated time; starts away from zero so a deadline in the past is
  // still a real time point.
  uint64_t now_ms() const noexcept { return _now; }

  // Runs `task` once `ms` of simulated time have passed; events at one time run in the
  // order they were scheduled.
  void after(uint64_t ms, std::function<void()> task) { _events.emplace(std::make_pair(_now + ms, _sequence++), std::move(task)); }

  // One step: a ready handler if there is one, else the next event, moving time to it.
  // False when nothing is left to do.
  bool step() {
    io.restart();
    if (io.poll_one() > 0) return true;
    if (_events.empty()) return false;
    auto next = _events.begin();
    _now = next->first.first;
    auto task = std::move(next->second);
    _events.erase(next);
    task();
    return true;
  }

  // Steps until `done` holds, or `max_steps` have run, or nothing is left; whether `done`
  // held.
  template <class Done>
  bool run_until(Done&& done, uint64_t max_steps) {
    for (uint64_t i = 0; i < max_steps; ++i) {
      if (done()) return true;
      if (!step()) return done();
    }
    return done();
  }

 private:
  uint64_t _now = 1'000'000;
  uint64_t _sequence = 0;
  std::multimap<std::pair<uint64_t, uint64_t>, std::function<void()>> _events;
};

// The world's time, as a node's TimeSource. A waiter never needs to look again: the
// world's schedule wakes it exactly.
class Time final : public contract::TimeSource {
 public:
  explicit Time(const World& world) noexcept : _world(world) {}
  contract::TimePoint now() const noexcept override { return contract::TimePoint(std::chrono::milliseconds(_world.now_ms())); }
  std::chrono::milliseconds wake_within(std::chrono::milliseconds remaining) const noexcept override { return remaining; }

 private:
  const World& _world;
};

}  // namespace dannan::test::sim
