//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace dannan::test {

// A test proves another thread has reached a point, and holds it there,
// instead of sleeping and hoping: the thread calls enter_and_wait(), the test
// waits for the entries it expects, inspects, then opens the gate. Opening is
// permanent; later entries pass straight through.
class Gate {
 public:
  void enter_and_wait() {
    std::unique_lock lock(_mutex);
    ++_entered;
    _changed.notify_all();
    _changed.wait(lock, [&] { return _open; });
  }

  // True once `count` threads have entered; false if they have not within
  // `timeout`.
  bool wait_for_entries(size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::unique_lock lock(_mutex);
    return _changed.wait_for(lock, timeout, [&] { return _entered >= count; });
  }

  size_t entered() const {
    std::lock_guard lock(_mutex);
    return _entered;
  }

  void open() {
    std::lock_guard lock(_mutex);
    _open = true;
    _changed.notify_all();
  }

 private:
  mutable std::mutex _mutex;
  std::condition_variable _changed;
  size_t _entered = 0;
  bool _open = false;
};

// Opens the gate when it leaves scope, so a failed assertion never leaves a
// thread blocked inside it.
class GateOpener {
 public:
  explicit GateOpener(Gate& gate) : _gate(gate) {}
  ~GateOpener() { _gate.open(); }
  GateOpener(const GateOpener&) = delete;
  GateOpener& operator=(const GateOpener&) = delete;

 private:
  Gate& _gate;
};

}  // namespace dannan::test
