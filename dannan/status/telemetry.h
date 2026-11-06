//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/membership/identity_reset.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/codec.h"

namespace dannan::status {

// Startup's phases, as Status names them. A record that omits its phase
// reads as ready, so a record always states it.
enum class Phase : uint8_t { starting, recovering, ready };
const char* phase_name(Phase) noexcept;

// Dannan's field numbers: the core ones are below first_hosted and filled
// by Dannan; a hosted system numbers its own from first_hosted, and they
// are carried as they come.
namespace field {
inline constexpr uint16_t node_id = 1;
inline constexpr uint16_t boot_id = 2;
inline constexpr uint16_t sequence = 3;
inline constexpr uint16_t observed_unix_ms = 4;
inline constexpr uint16_t version = 5;
inline constexpr uint16_t host = 6;
inline constexpr uint16_t failure_domain = 7;
inline constexpr uint16_t port = 8;
inline constexpr uint16_t uptime_ms = 9;
inline constexpr uint16_t rss_bytes = 10;
inline constexpr uint16_t process_cpu_milli_percent = 11;
inline constexpr uint16_t load1_milli = 12;
inline constexpr uint16_t peers_known = 13;
inline constexpr uint16_t peers_active = 14;
inline constexpr uint16_t connections_created = 15;
inline constexpr uint16_t connections_reused = 16;
inline constexpr uint16_t connections_canonical = 17;
inline constexpr uint16_t phase = 18;
inline constexpr uint16_t cpu_cores = 19;
inline constexpr uint16_t memory_total_bytes = 20;
inline constexpr uint16_t node_name = 21;
inline constexpr uint16_t traffic = 22;
inline constexpr uint16_t first_hosted = 1024;
}  // namespace field

// One frame class's traffic: totals since start and the rate over the last
// interval.
struct TrafficClass {
  uint8_t frame_class = 0;
  uint64_t in_bytes = 0;
  uint64_t out_bytes = 0;
  uint32_t in_bytes_per_s = 0;
  uint32_t out_bytes_per_s = 0;
  bool operator==(const TrafficClass&) const = default;
};

// A node's telemetry record: tagged fields in order. Nothing is
// positional: an unknown field is kept as it came, a missing one reads as
// its default ("did not say"), and a default value is never written, so
// versions can differ and a record stays gossip-sized.
class Telemetry {
 public:
  using Field = std::pair<uint16_t, net::Bytes>;

  // Each replaces the field in place, or appends it; a default value (zero,
  // empty) removes it.
  void set_bytes(uint16_t id, std::span<const uint8_t> value);
  void set_u64(uint16_t id, uint64_t value);
  void set_u32(uint16_t id, uint32_t value);
  void set_u16(uint16_t id, uint16_t value);
  void set_u8(uint16_t id, uint8_t value);
  void set_text(uint16_t id, const std::string& value);
  // Always stated, even as starting.
  void set_phase(Phase value);
  void set_traffic(uint32_t window_ms, const std::vector<TrafficClass>& classes);

  // Absent reads as zero or empty. A value of the wrong width throws
  // net::DecodeError.
  std::optional<std::span<const uint8_t>> get(uint16_t id) const;
  uint64_t u64(uint16_t id) const;
  uint32_t u32(uint16_t id) const;
  uint16_t u16(uint16_t id) const;
  uint8_t u8(uint16_t id) const;
  std::string text(uint16_t id) const;

  identity::NodeId node_id() const;
  identity::NodeId boot_id() const;
  uint64_t sequence() const { return u64(field::sequence); }
  uint64_t observed_unix_ms() const { return u64(field::observed_unix_ms); }
  std::string host() const { return text(field::host); }
  uint16_t port() const { return u16(field::port); }
  Phase phase() const;
  uint32_t traffic_window_ms() const;
  std::vector<TrafficClass> traffic() const;

  // Puts the fields `layout` names first, in its order; the rest keep
  // theirs, after.
  void order_by(const std::vector<uint16_t>& layout);
  const std::vector<Field>& fields() const noexcept { return _fields; }

 private:
  std::vector<Field> _fields;
};

// The encodings (DNNTEL01): one record, or a set of them.
// Decoding throws net::DecodeError for a bad magic, a core field of the
// wrong width, a phase out of range, a zero sequence, or over 65536
// records.
net::Bytes encode_telemetry(const Telemetry& record);
Telemetry decode_telemetry(std::span<const uint8_t> bytes);
net::Bytes encode_telemetry_set(const std::vector<Telemetry>& records);
std::vector<Telemetry> decode_telemetry_set(std::span<const uint8_t> bytes);

// The transport's totals by frame class, as TransportTraffic counts them.
struct TrafficTotals {
  std::array<uint64_t, 6> in_bytes{};
  std::array<uint64_t, 6> out_bytes{};
};

// What the node knows of itself for its record.
struct LocalFigures {
  membership::NodeRecord self;
  std::string version;
  Phase phase = Phase::starting;
  uint32_t peers_known = 0;
  uint32_t peers_active = 0;
  uint64_t connections_created = 0;
  uint64_t connections_reused = 0;
  uint64_t connections_canonical = 0;
  std::string node_name;
  std::optional<TrafficTotals> traffic;
};

struct TelemetryView {
  Telemetry telemetry;
  std::chrono::milliseconds age{};
  bool fresh = false;
};

// Every node's latest record, this node's included. Thread-safe; waits
// only in persist(), on the file.
class TelemetryStore {
 public:
  // `persisted_path` empty: nothing persisted. A persisted file that cannot
  // be read is ignored: telemetry is observational, never a reason not to
  // start.
  TelemetryStore(identity::NodeId self, std::filesystem::path persisted_path = {}, const contract::TimeSource* time = &contract::steady_time_source(),
                 const contract::WallClock* wall = &contract::system_wall_clock(), std::vector<uint16_t> layout = {});
  TelemetryStore(const TelemetryStore&) = delete;
  // Why the persisted records could not be read; empty if they could.
  const std::string& load_error() const noexcept { return _load_error; }
  TelemetryStore& operator=(const TelemetryStore&) = delete;

  // Builds this node's next record: the core fields from `figures` and the
  // process, the hosted system's from `extra`; the fields `layout` names
  // first, in its order, the rest as set. Stored and returned.
  Telemetry refresh_local(const LocalFigures& figures, const std::function<void(Telemetry&)>& extra = {});

  // A record heard of. Within one boot a higher sequence wins; a new boot
  // wins unless it is over a minute older. A record without a node id or a
  // sequence is ignored, as is one an identity reset covers unless `direct`
  // or newer than the reset.
  void observe(Telemetry record, bool direct = false);
  void apply_identity_reset(const membership::IdentityReset& reset);

  std::optional<Telemetry> local() const;
  std::vector<Telemetry> all() const;
  // Received within `max_age`, newest observed first, at most `max_records`.
  std::vector<Telemetry> recent(std::chrono::milliseconds max_age, size_t max_records) const;
  std::vector<TelemetryView> views(std::chrono::milliseconds fresh_for) const;
  // The last persisted set, read at start and after each persist().
  std::vector<Telemetry> persisted() const;
  // Writes the newest record per node, persisted and live merged, at most
  // 1024. Waits on the file.
  void persist();

 private:
  struct Received {
    Telemetry telemetry;
    contract::TimePoint at;
  };

  const identity::NodeId _self;
  const identity::NodeId _boot;
  const std::filesystem::path _path;
  const contract::TimeSource* const _time;
  const contract::WallClock* const _wall;
  const std::vector<uint16_t> _layout;
  const contract::TimePoint _started;

  std::string _load_error;
  mutable contract::Mutex _mutex;
  uint64_t _sequence DANNAN_GUARDED_BY(_mutex) = 0;
  std::unordered_map<identity::NodeId, Received, identity::NodeIdHash> _records DANNAN_GUARDED_BY(_mutex);
  std::map<identity::NodeId, Telemetry> _persisted DANNAN_GUARDED_BY(_mutex);
  std::unordered_map<std::string, membership::IdentityReset> _resets DANNAN_GUARDED_BY(_mutex);
  std::optional<TrafficTotals> _previous_traffic DANNAN_GUARDED_BY(_mutex);
  contract::TimePoint _previous_traffic_at DANNAN_GUARDED_BY(_mutex){};
  std::clock_t _previous_cpu DANNAN_GUARDED_BY(_mutex);
  contract::TimePoint _previous_cpu_at DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::status
