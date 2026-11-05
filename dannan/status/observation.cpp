//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/observation.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <mutex>
#include <utility>

namespace dannan::status {
namespace {

uint64_t minus(uint64_t later, uint64_t earlier) noexcept { return later >= earlier ? later - earlier : 0; }

size_t octave_of(size_t bucket) noexcept { return (bucket - 8) / sub_buckets + 3; }

bool route_word(std::string_view segment) noexcept {
  if (segment.empty() || segment.size() > 32) return false;
  return std::all_of(segment.begin(), segment.end(), [](char c) { return (c >= 'a' && c <= 'z') || c == '_' || c == '-'; });
}

// A minimal JSON writer: compact, keys in the order given (callers sort).
void quote(std::string& out, std::string_view text) {
  static constexpr char hex[] = "0123456789abcdef";
  out.push_back('"');
  for (const unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xf]);
          out.push_back(hex[c & 0xf]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

// Members in sorted key order: the caller adds them in any order.
class Object {
 public:
  Object& number(std::string key, uint64_t value) {
    _members[std::move(key)] = std::to_string(value);
    return *this;
  }
  Object& text(std::string key, std::string_view value) {
    std::string out;
    quote(out, value);
    _members[std::move(key)] = std::move(out);
    return *this;
  }
  Object& raw(std::string key, std::string json) {
    _members[std::move(key)] = std::move(json);
    return *this;
  }
  std::string str() const {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : _members) {
      if (!first) out.push_back(',');
      first = false;
      quote(out, key);
      out.push_back(':');
      out += value;
    }
    out.push_back('}');
    return out;
  }

 private:
  std::map<std::string, std::string> _members;
};

}  // namespace

size_t bucket_of(uint64_t value) noexcept {
  if (value < 8) return static_cast<size_t>(value);
  const auto octave = static_cast<size_t>(63 - std::countl_zero(value));
  const auto sub = static_cast<size_t>((value >> (octave - 3)) & 7u);
  return 8 + (octave - 3) * sub_buckets + sub;
}

uint64_t bucket_lower(size_t bucket) noexcept {
  if (bucket < 8) return bucket;
  return static_cast<uint64_t>(8 + (bucket - 8) % sub_buckets) << (octave_of(bucket) - 3);
}

uint64_t bucket_upper(size_t bucket) noexcept {
  if (bucket < 8) return bucket;
  return bucket_lower(bucket) + ((uint64_t{1} << (octave_of(bucket) - 3)) - 1);
}

uint64_t HistogramSnapshot::quantile(double q) const noexcept {
  if (!count) return 0;
  q = std::clamp(q, 0.0, 1.0);
  const auto rank = std::clamp<uint64_t>(static_cast<uint64_t>(std::ceil(q * static_cast<double>(count))), 1, count);
  uint64_t seen = 0;
  for (size_t bucket = 0; bucket < buckets.size(); ++bucket) {
    seen += buckets[bucket];
    if (seen >= rank) return std::min(bucket_upper(bucket), max);
  }
  return max;
}

HistogramSnapshot HistogramSnapshot::since(const HistogramSnapshot& earlier) const noexcept {
  HistogramSnapshot window;
  size_t highest = buckets.size();
  for (size_t bucket = 0; bucket < buckets.size(); ++bucket) {
    window.buckets[bucket] = minus(buckets[bucket], earlier.buckets[bucket]);
    window.count += window.buckets[bucket];
    if (window.buckets[bucket]) highest = bucket;
  }
  window.sum = minus(sum, earlier.sum);
  if (highest != buckets.size()) window.max = std::min(bucket_upper(highest), max);
  return window;
}

void Histogram::record(uint64_t value) noexcept {
  _buckets[bucket_of(value)].fetch_add(1, std::memory_order_relaxed);
  _sum.fetch_add(value, std::memory_order_relaxed);
  auto seen = _max.load(std::memory_order_relaxed);
  while (seen < value && !_max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

HistogramSnapshot Histogram::snapshot() const noexcept {
  HistogramSnapshot out;
  for (size_t bucket = 0; bucket < _buckets.size(); ++bucket) {
    out.buckets[bucket] = _buckets[bucket].load(std::memory_order_relaxed);
    out.count += out.buckets[bucket];
  }
  out.sum = _sum.load(std::memory_order_relaxed);
  out.max = _max.load(std::memory_order_relaxed);
  return out;
}

ObservationSnapshot ObservationSnapshot::since(const ObservationSnapshot& earlier) const {
  ObservationSnapshot window;
  for (const auto& [name, histogram] : histograms) {
    const auto before = earlier.histograms.find(name);
    window.histograms.emplace(name, histogram.since(before == earlier.histograms.end() ? HistogramSnapshot{} : before->second));
  }
  for (const auto& [name, value] : counters) {
    const auto before = earlier.counters.find(name);
    window.counters.emplace(name, minus(value, before == earlier.counters.end() ? 0 : before->second));
  }
  return window;
}

Histogram& Observations::histogram(std::string_view name) {
  contract::Lock lock(_mutex);
  if (const auto found = _histograms.find(name); found != _histograms.end()) return *found->second;
  if (_histograms.size() >= _max_series) {
    _overflowed.fetch_add(1, std::memory_order_relaxed);
    return _overflow_histogram;
  }
  return *_histograms.emplace(std::string(name), std::make_unique<Histogram>()).first->second;
}

std::atomic<uint64_t>& Observations::counter(std::string_view name) {
  contract::Lock lock(_mutex);
  if (const auto found = _counters.find(name); found != _counters.end()) return *found->second;
  if (_counters.size() >= _max_series) {
    _overflowed.fetch_add(1, std::memory_order_relaxed);
    return _overflow_counter;
  }
  return *_counters.emplace(std::string(name), std::make_unique<std::atomic<uint64_t>>(0)).first->second;
}

void Observations::event(ObservationEvent event) {
  contract::Lock lock(_mutex);
  if (_events.size() >= _max_events) {
    _events.pop_front();
    _events_dropped.fetch_add(1, std::memory_order_relaxed);
  }
  _events.push_back(std::move(event));
}

ObservationSnapshot Observations::snapshot() const {
  ObservationSnapshot out;
  contract::Lock lock(_mutex);
  for (const auto& [name, histogram] : _histograms) out.histograms.emplace(name, histogram->snapshot());
  for (const auto& [name, counter] : _counters) out.counters.emplace(name, counter->load(std::memory_order_relaxed));
  out.histograms.emplace(std::string(overflow_series), _overflow_histogram.snapshot());
  out.counters.emplace(std::string(overflow_series), _overflowed.load(std::memory_order_relaxed));
  out.counters.emplace(std::string(events_dropped_series), _events_dropped.load(std::memory_order_relaxed));
  return out;
}

std::deque<ObservationEvent> Observations::drain_events() {
  contract::Lock lock(_mutex);
  return std::exchange(_events, {});
}

std::string route_label(std::string_view method, std::string_view path) {
  std::string label = "api ";
  label += method == "GET" || method == "POST" || method == "PUT" || method == "PATCH" || method == "DELETE" ? std::string(method) : std::string("OTHER");
  if (!path.starts_with("/api/")) return label + " web";
  if (!path.starts_with("/api/v1/")) return label + " /api/:other";
  label += " /api/v1";
  auto rest = path.substr(8);
  for (size_t kept = 0; kept < 3 && !rest.empty(); ++kept) {
    const auto slash = rest.find('/');
    const auto segment = rest.substr(0, slash);
    label += '/';
    label += route_word(segment) ? std::string(segment) : std::string(":id");
    rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
  }
  return label;
}

std::string render_window(uint64_t start_unix_ms, uint64_t end_unix_ms, std::string_view version, const ObservationSnapshot& window,
                          const std::map<std::string, uint64_t>& gauges) {
  Object histograms;
  for (const auto& [name, histogram] : window.histograms) {
    if (!histogram.count) continue;
    std::string buckets = "[";
    bool first = true;
    for (size_t bucket = 0; bucket < histogram.buckets.size(); ++bucket) {
      if (!histogram.buckets[bucket]) continue;
      if (!first) buckets.push_back(',');
      first = false;
      buckets += "[" + std::to_string(bucket) + "," + std::to_string(histogram.buckets[bucket]) + "]";
    }
    buckets.push_back(']');
    histograms.raw(name, Object()
                             .number("count", histogram.count)
                             .number("sum", histogram.sum)
                             .number("max", histogram.max)
                             .number("p50", histogram.quantile(0.5))
                             .number("p90", histogram.quantile(0.9))
                             .number("p99", histogram.quantile(0.99))
                             .raw("buckets", buckets)
                             .str());
  }
  Object counters;
  for (const auto& [name, value] : window.counters)
    if (value) counters.number(name, value);
  Object gauge_values;
  for (const auto& [name, value] : gauges) gauge_values.number(name, value);
  return Object()
      .text("kind", "window")
      .number("start_ms", start_unix_ms)
      .number("end_ms", end_unix_ms)
      .text("version", version)
      .raw("histograms", histograms.str())
      .raw("counters", counters.str())
      .raw("gauges", gauge_values.str())
      .str();
}

std::string render_event(const ObservationEvent& event) {
  Object fields;
  for (const auto& [name, value] : event.numbers) fields.number(name, value);
  for (const auto& [name, value] : event.texts) fields.text(name, value);
  return Object().text("kind", "event").number("at_ms", event.unix_ms).text("event", event.name).raw("fields", fields.str()).str();
}

void ObservationLog::append(const std::string& line) {
  contract::Lock lock(_mutex);
  std::error_code ignored;
  std::filesystem::create_directories(_path.parent_path(), ignored);
  std::error_code missing;
  const auto size = std::filesystem::file_size(_path, missing);
  if (!missing && size > 0 && size + line.size() + 1 > _max_bytes) {
    auto rotated = _path;
    rotated += ".1";
    std::filesystem::rename(_path, rotated, ignored);
  }
  std::ofstream out(_path, std::ios::app | std::ios::binary);
  out << line << '\n';
  out.flush();
  _failing = !out;
}

ObservationRecorder::ObservationRecorder(Observations& registry, std::shared_ptr<ObservationLog> log, std::string version, std::chrono::milliseconds interval,
                                         Gauges gauges, const contract::WallClock* wall, supervise::SupervisedThreads* threads)
    : _registry(registry),
      _log(std::move(log)),
      _version(std::move(version)),
      _interval(interval),
      _gauges(std::move(gauges)),
      _wall(wall),
      _threads(threads) {}

ObservationRecorder::~ObservationRecorder() { stop(); }

void ObservationRecorder::start() {
  {
    contract::Lock lock(_tick_mutex);
    _previous_unix_ms = _wall->unix_ms();
  }
  _thread = std::jthread([this](std::stop_token stop) {
    supervise::supervised_loop(_threads, "observation-recorder", stop, [this, stop] {
      while (!stop.stop_requested()) {
        {
          std::unique_lock lock(_wait_mutex);
          _wake.wait_for(lock, stop, _interval, [] { return false; });
        }
        if (stop.stop_requested()) return;
        tick(_wall->unix_ms());
      }
    });
  });
}

void ObservationRecorder::stop() {
  if (!_thread.joinable()) return;
  _thread.request_stop();
  _wake.notify_all();
  _thread.join();
  tick(_wall->unix_ms());
}

void ObservationRecorder::tick(uint64_t now_unix_ms) {
  contract::Lock lock(_tick_mutex);
  auto current = _registry.snapshot();
  std::map<std::string, uint64_t> gauges;
  if (_gauges) {
    try {
      gauges = _gauges();
    } catch (const std::exception&) {
      // A failed sample loses the gauges, not the window.
    }
  }
  for (const auto& event : _registry.drain_events()) _log->append(render_event(event));
  _log->append(render_window(_previous_unix_ms, now_unix_ms, _version, current.since(_previous), gauges));
  _previous = std::move(current);
  _previous_unix_ms = now_unix_ms;
}

}  // namespace dannan::status
