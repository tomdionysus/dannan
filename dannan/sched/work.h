//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/status/log.h"

namespace dannan::sched {

using contract::Clock;
using contract::TimePoint;

// How work travels: the wire encoding of its class, with read-ahead as a hint
// inside the foreground class. The values are protocol 23's and never change.
enum class FrameType : uint8_t {
  control = 1,
  foreground = 2,
  read_ahead = 3,
  speculative = 4,
  loader = 5,  // bulk work someone asked for
};

// The frame type a wire byte names, or none for any other value.
std::optional<FrameType> decode_frame_type(uint8_t value) noexcept;
const char* frame_type_name(FrameType) noexcept;

// Order on a transport queue, lower first: control, foreground, read-ahead,
// loader, speculative (laws 1 to 4).
constexpr unsigned frame_priority(FrameType type) noexcept {
  switch (type) {
    case FrameType::control:
      return 0;
    case FrameType::foreground:
      return 1;
    case FrameType::read_ahead:
      return 2;
    case FrameType::loader:
      return 3;
    case FrameType::speculative:
      return 4;
  }
  return 4;
}

// What work is, by the laws' ranking: control (law 1), foreground (law 2: a
// person is waiting), loader (law 3), then speculative (repair, prefetch,
// maintenance). The one definition: everything that admits, paces or measures
// work by class reads work_class(), and nothing decides class from a frame
// itself. A hosted system may call foreground something else; Dannan does not.
enum class WorkClass : uint8_t { control, foreground, loader, speculative };

constexpr WorkClass work_class(FrameType type) noexcept {
  switch (type) {
    case FrameType::control:
      return WorkClass::control;
    case FrameType::foreground:
    case FrameType::read_ahead:
      return WorkClass::foreground;
    case FrameType::loader:
      return WorkClass::loader;
    case FrameType::speculative:
      return WorkClass::speculative;
  }
  return WorkClass::speculative;
}

// The frame a class's work travels on: foreground's is foreground, never
// read-ahead. Control moves no DATA; DATA admission refuses it.
constexpr FrameType frame_for(WorkClass work) noexcept {
  switch (work) {
    case WorkClass::control:
      return FrameType::control;
    case WorkClass::foreground:
      return FrameType::foreground;
    case WorkClass::loader:
      return FrameType::loader;
    case WorkClass::speculative:
      return FrameType::speculative;
  }
  return FrameType::speculative;
}

const char* work_class_name(WorkClass) noexcept;

// What an operation may wait on, declared on its contract beside the
// operation (`static constexpr Waits x_waits`). `locks` means it takes a lock
// something holds across I/O, which makes that I/O its wait too.
enum class Waits : uint8_t {
  none = 0,
  state_device = 1,
  data_device = 2,
  network = 4,
  locks = 8,
};

constexpr Waits operator|(Waits a, Waits b) noexcept { return static_cast<Waits>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
constexpr bool includes(Waits declared, Waits wait) noexcept { return (static_cast<uint8_t>(declared) & static_cast<uint8_t>(wait)) != 0; }

// Waits on this node alone: its state device and its locks, never the DATA
// device or a peer.
inline constexpr Waits local_waits = Waits::state_device | Waits::locks;

// What a class's work may wait on unless it allows itself less: control
// never on the DATA device or the network (law 1); every other class on
// anything.
constexpr Waits default_waits(WorkClass work) noexcept {
  return work == WorkClass::control ? local_waits : Waits::state_device | Waits::data_device | Waits::network | Waits::locks;
}

// Whether work allowed `allowed` may enter an operation declaring `declared`:
// every wait the operation may make is one the work allows.
constexpr bool may_enter(Waits allowed, Waits declared) noexcept { return (static_cast<uint8_t>(declared) & ~static_cast<uint8_t>(allowed)) == 0; }

// Who may call an operation, declared beside its waits (`x_safety`):
// `thread_safe` from any thread; `single_owner` from the one caller named
// where the operation is declared.
enum class ThreadSafety : uint8_t { thread_safe, single_owner };

// Who is asking, what class of work it is, what it may wait on, until when,
// and whether they have given up.
class WorkContext {
 public:
  // `origin` names who is asking, a string with static storage the wait
  // guard reports. `allowed` narrows what the work may wait on; it never
  // widens the class's default. A default deadline (the epoch) is none.
  explicit WorkContext(FrameType type = FrameType::loader, TimePoint deadline = {}, std::atomic_bool* cancelled = nullptr, const char* origin = "unnamed",
                       std::optional<Waits> allowed = std::nullopt) noexcept
      : _type(type),
        _deadline(deadline),
        _cancelled(cancelled),
        _origin(origin),
        _allowed(static_cast<Waits>(static_cast<uint8_t>(default_waits(work_class(type))) &
                                    static_cast<uint8_t>(allowed.value_or(default_waits(work_class(type)))))) {}

  FrameType frame_type() const noexcept { return _type; }
  WorkClass work() const noexcept { return work_class(_type); }
  Waits allowed() const noexcept { return _allowed; }
  const char* origin() const noexcept { return _origin; }
  TimePoint deadline() const noexcept { return _deadline; }
  std::atomic_bool* cancellation() const noexcept { return _cancelled; }
  bool cancelled() const noexcept { return _cancelled && _cancelled->load(std::memory_order_relaxed); }
  // At `now`, from the component's injected time source.
  bool expired(TimePoint now) const noexcept { return _deadline != TimePoint{} && now >= _deadline; }

 private:
  FrameType _type;
  TimePoint _deadline;
  std::atomic_bool* _cancelled;
  const char* _origin;
  Waits _allowed;
};

// The runtime check at a contract boundary, called by each operation that
// declares a device or network wait. One per node, handed to the components
// that check. A refusal is counted every time and reported once per (origin,
// operation); tests set throw_on_violation.
class WaitGuard {
 public:
  enum class Mode { record, throw_on_violation };

  explicit WaitGuard(status::ScopedLogger logger, Mode mode = Mode::record) : _logger(std::move(logger)), _mode(mode) {}

  // Whether `context` may enter `operation`, which declares `declared`.
  // Throws std::logic_error on a refusal in throw_on_violation mode.
  // Waits on nothing but the log sink, once per path; thread-safe.
  bool enter(const WorkContext& context, Waits declared, std::string_view operation);

  void set_mode(Mode mode) noexcept { _mode.store(mode, std::memory_order_relaxed); }
  Mode mode() const noexcept { return _mode.load(std::memory_order_relaxed); }
  uint64_t violations() const noexcept { return _violations.load(std::memory_order_relaxed); }

 private:
  status::ScopedLogger _logger;
  std::atomic<Mode> _mode;
  std::atomic<uint64_t> _violations{0};
  contract::Mutex _mutex;
  std::set<std::string, std::less<>> _reported DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::sched
