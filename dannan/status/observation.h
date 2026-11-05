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
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/supervise/supervised.h"

// Measurement probes, written only to a local file (one JSON line per
// window, protocol 23's observations.jsonl). Nothing here reaches Status or
// the wire, and no probe changes a decision: each is a counter, a histogram
// record or a bounded event.
namespace dannan::status {

// Log-linear buckets: 0 to 7 one each, then every power of two from 8 split
// into eight, so any quantile is within 12.5% of a recorded value. 496
// buckets cover every uint64_t, contiguously.
inline constexpr size_t sub_buckets = 8;
inline constexpr size_t bucket_count = 8 + 61 * sub_buckets;
size_t bucket_of(uint64_t value) noexcept;
// Precondition: bucket < bucket_count.
uint64_t bucket_lower(size_t bucket) noexcept;
uint64_t bucket_upper(size_t bucket) noexcept;

// A histogram at one moment, or over a window (two moments' difference).
struct HistogramSnapshot {
  std::array<uint64_t, bucket_count> buckets{};
  uint64_t count = 0;
  uint64_t sum = 0;
  uint64_t max = 0;

  // The least bucket upper bound at or above which a fraction q of values
  // lie, capped at max; 0 when empty.
  uint64_t quantile(double q) const noexcept;
  // This less an earlier snapshot of the same histogram; the window's max is
  // its highest non-empty bucket's upper bound, capped at max.
  HistogramSnapshot since(const HistogramSnapshot& earlier) const noexcept;
  bool operator==(const HistogramSnapshot&) const = default;
};

// Lock-free; record() is safe from any thread.
class Histogram {
 public:
  void record(uint64_t value) noexcept;
  HistogramSnapshot snapshot() const noexcept;

 private:
  std::array<std::atomic<uint64_t>, bucket_count> _buckets{};
  std::atomic<uint64_t> _sum{0};
  std::atomic<uint64_t> _max{0};
};

// A one-off fact with its own time.
struct ObservationEvent {
  uint64_t unix_ms = 0;
  std::string name;
  std::map<std::string, uint64_t> numbers;
  std::map<std::string, std::string> texts;
  bool operator==(const ObservationEvent&) const = default;
};

struct ObservationSnapshot {
  std::map<std::string, HistogramSnapshot, std::less<>> histograms;
  std::map<std::string, uint64_t, std::less<>> counters;
  // A series the earlier lacks counts from zero.
  ObservationSnapshot since(const ObservationSnapshot& earlier) const;
};

// Named histograms and counters, and a bounded queue of events. At most
// max_series of each; a name beyond that folds into overflow_series and is
// counted there, so a label made from input cannot grow memory. At most
// max_events wait; beyond that the oldest is dropped and counted. Series
// are never removed; a returned reference lives as long as the registry.
// Thread-safe. One per node, injected: there is no process registry.
class Observations {
 public:
  static constexpr std::string_view overflow_series = "observation.overflow";
  static constexpr std::string_view events_dropped_series = "observation.events_dropped";

  explicit Observations(size_t max_series = 512, size_t max_events = 256) : _max_series(max_series), _max_events(max_events) {}

  Histogram& histogram(std::string_view name);
  std::atomic<uint64_t>& counter(std::string_view name);
  void add(std::string_view name, uint64_t amount = 1) { counter(name).fetch_add(amount, std::memory_order_relaxed); }
  void record(std::string_view name, uint64_t value) { histogram(name).record(value); }
  void event(ObservationEvent event);
  ObservationSnapshot snapshot() const;
  // Every waiting event, oldest first.
  std::deque<ObservationEvent> drain_events();

 private:
  const size_t _max_series;
  const size_t _max_events;
  mutable contract::Mutex _mutex;
  std::map<std::string, std::unique_ptr<Histogram>, std::less<>> _histograms DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, std::unique_ptr<std::atomic<uint64_t>>, std::less<>> _counters DANNAN_GUARDED_BY(_mutex);
  std::deque<ObservationEvent> _events DANNAN_GUARDED_BY(_mutex);
  Histogram _overflow_histogram;
  std::atomic<uint64_t> _overflow_counter{0};
  std::atomic<uint64_t> _overflowed{0};
  std::atomic<uint64_t> _events_dropped{0};
};

// Times a span into a histogram of microseconds: from construction to
// destruction, or to stop(), on the injected clock. For a store's barrier
// and I/O times and the like. Not thread-safe; one span, one owner.
class ObservedDuration {
 public:
  ObservedDuration(Observations& observations, std::string_view series, const contract::TimeSource* time = &contract::steady_time_source())
      : _observations(&observations), _series(series), _time(time), _started(time->now()) {}
  ~ObservedDuration() { stop(); }
  ObservedDuration(const ObservedDuration&) = delete;
  ObservedDuration& operator=(const ObservedDuration&) = delete;

  uint64_t elapsed_us() const { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(_time->now() - _started).count()); }
  // Records now, once; the destructor then records nothing.
  void stop() {
    if (!_observations) return;
    _observations->record(_series, elapsed_us());
    _observations = nullptr;
  }
  // Records nothing (a span abandoned).
  void cancel() noexcept { _observations = nullptr; }

 private:
  Observations* _observations;
  std::string _series;
  const contract::TimeSource* _time;
  contract::TimePoint _started;
};

// "api " + method + route: under /api/v1/ at most three segments, each kept
// if 1-32 of [a-z_-] and ":id" otherwise; outside /api/ "web"; methods other
// than the usual five "OTHER".
std::string route_label(std::string_view method, std::string_view path);

// One JSON line per window (series that did not move omitted; each
// histogram with count, sum, max, p50, p90, p99 and its non-empty buckets)
// and per event. Keys sorted, compact: equal inputs render equal text.
std::string render_window(uint64_t start_unix_ms, uint64_t end_unix_ms, std::string_view version, const ObservationSnapshot& window,
                          const std::map<std::string, uint64_t>& gauges);
std::string render_event(const ObservationEvent& event);

// Appends lines to one file, rotated to <path>.1 when a line would take it
// past max_bytes. Best effort: a failed write loses the line. Thread-safe;
// waits on the file.
class ObservationLog {
 public:
  ObservationLog(std::filesystem::path path, uint64_t max_bytes) : _path(std::move(path)), _max_bytes(max_bytes) {}
  const std::filesystem::path& path() const noexcept { return _path; }
  void append(const std::string& line);

 private:
  const std::filesystem::path _path;
  const uint64_t _max_bytes;
  // Held across the append and the rotation.
  contract::IoMutex _mutex;
  bool _failing DANNAN_GUARDED_BY(_mutex) = false;
};

// Writes a window every `interval` from its own thread: the movement since
// the last, the gauges sampled now, and the waiting events first. stop()
// writes a final window.
class ObservationRecorder {
 public:
  using Gauges = std::function<std::map<std::string, uint64_t>()>;

  ObservationRecorder(Observations& registry, std::shared_ptr<ObservationLog> log, std::string version, std::chrono::milliseconds interval, Gauges gauges,
                      const contract::WallClock* wall = &contract::system_wall_clock(), supervise::SupervisedThreads* threads = nullptr);
  ~ObservationRecorder();
  ObservationRecorder(const ObservationRecorder&) = delete;
  ObservationRecorder& operator=(const ObservationRecorder&) = delete;

  void start();
  void stop();
  // One window ending now; called by the thread, public for tests. Not
  // concurrently with itself.
  void tick(uint64_t now_unix_ms);

 private:
  Observations& _registry;
  const std::shared_ptr<ObservationLog> _log;
  const std::string _version;
  const std::chrono::milliseconds _interval;
  const Gauges _gauges;
  const contract::WallClock* const _wall;
  supervise::SupervisedThreads* const _threads;
  contract::IoMutex _tick_mutex;
  ObservationSnapshot _previous DANNAN_GUARDED_BY(_tick_mutex);
  uint64_t _previous_unix_ms DANNAN_GUARDED_BY(_tick_mutex) = 0;
  // Guards nothing: the thread's wait lock.
  std::mutex _wait_mutex;
  std::condition_variable_any _wake;
  std::jthread _thread;
};

}  // namespace dannan::status
