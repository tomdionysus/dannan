//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/status/telemetry.h"

#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <thread>

#include "dannan/identity/durable_file.h"
#include "dannan/status/process.h"

namespace dannan::status {
namespace {

constexpr std::array<uint8_t, 8> magic{'D', 'N', 'N', 'T', 'E', 'L', '0', '1'};
constexpr uint32_t max_set_records = 65536;
constexpr size_t max_persisted = 1024;
constexpr uint64_t max_persisted_bytes = 16ull * 1024 * 1024;

// The width each core field must have; zero for a text field.
std::optional<size_t> core_width(uint16_t id) {
  switch (id) {
    case field::node_id:
    case field::boot_id:
      return 16;
    case field::sequence:
    case field::observed_unix_ms:
    case field::uptime_ms:
    case field::rss_bytes:
    case field::connections_created:
    case field::connections_reused:
    case field::connections_canonical:
    case field::memory_total_bytes:
      return 8;
    case field::process_cpu_milli_percent:
    case field::load1_milli:
    case field::peers_known:
    case field::peers_active:
    case field::cpu_cores:
      return 4;
    case field::port:
      return 2;
    case field::phase:
      return 1;
    default:
      return std::nullopt;
  }
}

uint64_t big_endian(std::span<const uint8_t> value) {
  uint64_t out = 0;
  for (const auto byte : value) out = (out << 8) | byte;
  return out;
}

void encode_record(net::Writer& writer, const Telemetry& record) {
  net::Writer body;
  for (const auto& [id, value] : record.fields()) {
    if (value.size() > std::numeric_limits<uint16_t>::max()) throw std::runtime_error("telemetry field too large");
    body.u16(id);
    body.u16(static_cast<uint16_t>(value.size()));
    body.raw(value);
  }
  writer.u32(static_cast<uint32_t>(body.data().size()));
  writer.raw(body.data());
}

Telemetry decode_record(net::Reader& reader) {
  const auto length = reader.u32();
  net::Reader fields(reader.view(length));
  Telemetry out;
  while (fields.remaining()) {
    const auto id = fields.u16();
    const auto size = fields.u16();
    const auto value = fields.view(size);
    if (const auto width = core_width(id); width && *width != value.size())
      throw net::DecodeError("telemetry field " + std::to_string(id) + " has the wrong width");
    if (id == field::phase && value[0] > static_cast<uint8_t>(Phase::ready)) throw net::DecodeError("invalid telemetry node phase");
    // Kept exactly as sent, a zero included.
    out.set_bytes(id, value);
  }
  (void)out.traffic();  // validates the structured field
  if (!out.sequence()) throw net::DecodeError("telemetry sequence must be nonzero");
  return out;
}

uint32_t load1_milli() {
  double load = 0;
  if (getloadavg(&load, 1) != 1 || load < 0) return 0;
  return static_cast<uint32_t>(std::min<double>(load * 1000.0, std::numeric_limits<uint32_t>::max()));
}

}  // namespace

const char* phase_name(Phase phase) noexcept {
  switch (phase) {
    case Phase::starting:
      return "starting";
    case Phase::recovering:
      return "recovering";
    case Phase::ready:
      return "ready";
  }
  return "unknown";
}

void Telemetry::set_bytes(uint16_t id, std::span<const uint8_t> value) {
  const auto found = std::find_if(_fields.begin(), _fields.end(), [&](const Field& f) { return f.first == id; });
  if (found != _fields.end())
    found->second.assign(value.begin(), value.end());
  else
    _fields.emplace_back(id, net::Bytes(value.begin(), value.end()));
}

namespace {
void set_or_remove(std::vector<Telemetry::Field>& fields, uint16_t id) {
  std::erase_if(fields, [&](const Telemetry::Field& f) { return f.first == id; });
}
}  // namespace

void Telemetry::set_u64(uint16_t id, uint64_t value) {
  if (!value) return set_or_remove(_fields, id);
  net::Writer w;
  w.u64(value);
  set_bytes(id, w.data());
}

void Telemetry::set_u32(uint16_t id, uint32_t value) {
  if (!value) return set_or_remove(_fields, id);
  net::Writer w;
  w.u32(value);
  set_bytes(id, w.data());
}

void Telemetry::set_u16(uint16_t id, uint16_t value) {
  if (!value) return set_or_remove(_fields, id);
  net::Writer w;
  w.u16(value);
  set_bytes(id, w.data());
}

void Telemetry::set_u8(uint16_t id, uint8_t value) {
  if (!value) return set_or_remove(_fields, id);
  set_bytes(id, std::array<uint8_t, 1>{value});
}

void Telemetry::set_text(uint16_t id, const std::string& value) {
  if (value.empty()) return set_or_remove(_fields, id);
  set_bytes(id, {reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}

void Telemetry::set_phase(Phase value) { set_bytes(field::phase, std::array<uint8_t, 1>{static_cast<uint8_t>(value)}); }

void Telemetry::set_traffic(uint32_t window_ms, const std::vector<TrafficClass>& classes) {
  if (classes.empty()) return set_or_remove(_fields, field::traffic);
  net::Writer w;
  w.u32(window_ms);
  const auto count = std::min<size_t>(classes.size(), 16);
  w.u8(static_cast<uint8_t>(count));
  for (size_t i = 0; i < count; ++i) {
    w.u8(classes[i].frame_class);
    w.u64(classes[i].in_bytes);
    w.u64(classes[i].out_bytes);
    w.u32(classes[i].in_bytes_per_s);
    w.u32(classes[i].out_bytes_per_s);
  }
  set_bytes(field::traffic, w.data());
}

std::optional<std::span<const uint8_t>> Telemetry::get(uint16_t id) const {
  const auto found = std::find_if(_fields.begin(), _fields.end(), [&](const Field& f) { return f.first == id; });
  if (found == _fields.end()) return std::nullopt;
  return std::span<const uint8_t>(found->second);
}

namespace {
uint64_t read_uint(const std::optional<std::span<const uint8_t>>& value, size_t width, uint16_t id) {
  if (!value) return 0;
  if (value->size() != width) throw net::DecodeError("telemetry field " + std::to_string(id) + " has the wrong width");
  return big_endian(*value);
}
}  // namespace

uint64_t Telemetry::u64(uint16_t id) const { return read_uint(get(id), 8, id); }
uint32_t Telemetry::u32(uint16_t id) const { return static_cast<uint32_t>(read_uint(get(id), 4, id)); }
uint16_t Telemetry::u16(uint16_t id) const { return static_cast<uint16_t>(read_uint(get(id), 2, id)); }
uint8_t Telemetry::u8(uint16_t id) const { return static_cast<uint8_t>(read_uint(get(id), 1, id)); }

std::string Telemetry::text(uint16_t id) const {
  const auto value = get(id);
  return value ? std::string(value->begin(), value->end()) : std::string();
}

identity::NodeId Telemetry::node_id() const {
  identity::NodeId out;
  if (const auto value = get(field::node_id); value && value->size() == out.bytes.size()) std::copy(value->begin(), value->end(), out.bytes.begin());
  return out;
}

identity::NodeId Telemetry::boot_id() const {
  identity::NodeId out;
  if (const auto value = get(field::boot_id); value && value->size() == out.bytes.size()) std::copy(value->begin(), value->end(), out.bytes.begin());
  return out;
}

Phase Telemetry::phase() const {
  const auto value = get(field::phase);
  return value ? static_cast<Phase>(read_uint(value, 1, field::phase)) : Phase::ready;
}

uint32_t Telemetry::traffic_window_ms() const {
  const auto value = get(field::traffic);
  if (!value) return 0;
  net::Reader reader(*value);
  return reader.u32();
}

std::vector<TrafficClass> Telemetry::traffic() const {
  std::vector<TrafficClass> out;
  const auto value = get(field::traffic);
  if (!value) return out;
  net::Reader reader(*value);
  (void)reader.u32();
  const auto count = reader.u8();
  if (count > 16) throw net::DecodeError("too many traffic classes");
  for (uint8_t i = 0; i < count; ++i) {
    TrafficClass entry;
    entry.frame_class = reader.u8();
    entry.in_bytes = reader.u64();
    entry.out_bytes = reader.u64();
    entry.in_bytes_per_s = reader.u32();
    entry.out_bytes_per_s = reader.u32();
    out.push_back(entry);
  }
  reader.finish();
  return out;
}

void Telemetry::order_by(const std::vector<uint16_t>& layout) {
  std::map<uint16_t, size_t> rank;
  for (size_t i = 0; i < layout.size(); ++i) rank.emplace(layout[i], i);
  std::stable_sort(_fields.begin(), _fields.end(), [&](const Field& a, const Field& b) {
    const auto ra = rank.find(a.first);
    const auto rb = rank.find(b.first);
    const auto ka = ra == rank.end() ? layout.size() : ra->second;
    const auto kb = rb == rank.end() ? layout.size() : rb->second;
    return ka < kb;
  });
}

net::Bytes encode_telemetry(const Telemetry& record) {
  net::Writer writer;
  writer.raw(magic);
  encode_record(writer, record);
  return writer.take();
}

Telemetry decode_telemetry(std::span<const uint8_t> bytes) {
  net::Reader reader(bytes);
  if (reader.fixed<8>() != magic) throw net::DecodeError("bad telemetry payload");
  auto out = decode_record(reader);
  reader.finish();
  return out;
}

net::Bytes encode_telemetry_set(const std::vector<Telemetry>& records) {
  net::Writer writer;
  writer.raw(magic);
  writer.u32(static_cast<uint32_t>(records.size()));
  for (const auto& record : records) encode_record(writer, record);
  return writer.take();
}

std::vector<Telemetry> decode_telemetry_set(std::span<const uint8_t> bytes) {
  net::Reader reader(bytes);
  if (reader.fixed<8>() != magic) throw net::DecodeError("bad telemetry set");
  const auto count = reader.u32();
  if (count > max_set_records) throw net::DecodeError("too many telemetry records");
  std::vector<Telemetry> out;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i) out.push_back(decode_record(reader));
  reader.finish();
  return out;
}

TelemetryStore::TelemetryStore(identity::NodeId self, std::filesystem::path persisted_path, const contract::TimeSource* time, const contract::WallClock* wall,
                               std::vector<uint16_t> layout)
    : _self(self),
      _boot(identity::NodeId::random()),
      _path(std::move(persisted_path)),
      _time(time),
      _wall(wall),
      _layout(std::move(layout)),
      _started(time->now()) {
  contract::Lock lock(_mutex);
  _previous_cpu = std::clock();
  _previous_cpu_at = _started;
  if (_path.empty() || !std::filesystem::exists(_path)) return;
  try {
    const auto size = std::filesystem::file_size(_path);
    if (size > max_persisted_bytes) throw std::runtime_error("persisted telemetry is too large");
    std::ifstream input(_path, std::ios::binary);
    net::Bytes bytes(static_cast<size_t>(size));
    if (!input || (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))))
      throw std::runtime_error("cannot read persisted telemetry");
    auto records = decode_telemetry_set(bytes);
    if (records.size() > max_persisted) throw std::runtime_error("persisted telemetry has too many records");
    for (auto& record : records) {
      const auto node = record.node_id();
      const auto held = _persisted.find(node);
      if (held == _persisted.end() || held->second.observed_unix_ms() < record.observed_unix_ms()) _persisted[node] = std::move(record);
    }
  } catch (const std::exception&) {
    // Observational state only: a corrupt file never blocks a start.
    _persisted.clear();
  }
}

Telemetry TelemetryStore::refresh_local(const LocalFigures& figures, const std::function<void(Telemetry&)>& extra) {
  const auto now = _time->now();
  const auto cpu_now = std::clock();
  Telemetry t;
  {
    contract::Lock lock(_mutex);
    const auto wall_seconds = std::chrono::duration<double>(now - _previous_cpu_at).count();
    const auto cpu_seconds = static_cast<double>(cpu_now - _previous_cpu) / CLOCKS_PER_SEC;
    _previous_cpu = cpu_now;
    _previous_cpu_at = now;
    const auto percent = wall_seconds > 0 ? std::max(0.0, cpu_seconds / wall_seconds * 100.0) : 0.0;
    t.set_bytes(field::node_id, _self.bytes);
    t.set_bytes(field::boot_id, _boot.bytes);
    t.set_u64(field::sequence, ++_sequence);
    t.set_u64(field::observed_unix_ms, _wall->unix_ms());
    t.set_text(field::version, figures.version);
    t.set_text(field::host, figures.self.host);
    t.set_text(field::failure_domain, figures.self.failure_domain);
    t.set_u16(field::port, figures.self.port);
    t.set_u64(field::uptime_ms, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - _started).count()));
    t.set_u64(field::rss_bytes, resident_bytes());
    t.set_u32(field::process_cpu_milli_percent, static_cast<uint32_t>(std::min<double>(percent * 1000.0, std::numeric_limits<uint32_t>::max())));
    t.set_u32(field::load1_milli, load1_milli());
    t.set_u32(field::peers_known, figures.peers_known);
    t.set_u32(field::peers_active, figures.peers_active);
    t.set_u64(field::connections_created, figures.connections_created);
    t.set_u64(field::connections_reused, figures.connections_reused);
    t.set_u64(field::connections_canonical, figures.connections_canonical);
    t.set_phase(figures.phase);
    t.set_u32(field::cpu_cores, std::thread::hardware_concurrency());
    t.set_u64(field::memory_total_bytes, physical_memory_bytes());
    t.set_text(field::node_name, figures.node_name);
    if (figures.traffic) {
      // Rates over the interval since the previous sample; the first states
      // totals and no rate yet.
      const double seconds = _previous_traffic ? std::chrono::duration<double>(now - _previous_traffic_at).count() : 0.0;
      const auto rate = [&](uint64_t current, uint64_t previous) -> uint32_t {
        if (seconds <= 0 || current < previous) return 0;
        return static_cast<uint32_t>(std::min<double>(static_cast<double>(current - previous) / seconds, std::numeric_limits<uint32_t>::max()));
      };
      std::vector<TrafficClass> classes;
      for (size_t index = 1; index < figures.traffic->in_bytes.size(); ++index) {
        TrafficClass entry;
        entry.frame_class = static_cast<uint8_t>(index);
        entry.in_bytes = figures.traffic->in_bytes[index];
        entry.out_bytes = figures.traffic->out_bytes[index];
        if (_previous_traffic) {
          entry.in_bytes_per_s = rate(entry.in_bytes, _previous_traffic->in_bytes[index]);
          entry.out_bytes_per_s = rate(entry.out_bytes, _previous_traffic->out_bytes[index]);
        }
        classes.push_back(entry);
      }
      t.set_traffic(static_cast<uint32_t>(std::min<double>(seconds * 1000.0, std::numeric_limits<uint32_t>::max())), classes);
      _previous_traffic = figures.traffic;
      _previous_traffic_at = now;
    }
  }
  if (extra) extra(t);
  if (!_layout.empty()) t.order_by(_layout);
  observe(t, true);
  return t;
}

void TelemetryStore::observe(Telemetry record, bool direct) {
  const auto node = record.node_id();
  if (node == identity::NodeId{} || !record.sequence()) return;
  contract::Lock lock(_mutex);
  const auto host = record.host();
  const auto port = record.port();
  for (const auto& [_, reset] : _resets) {
    if (!reset.matches_endpoint(host, port) || !reset.matches_node(node)) continue;
    if (!direct && record.observed_unix_ms() <= reset.reset_unix_ms) return;
  }
  if (const auto held = _records.find(node); held != _records.end()) {
    const auto& current = held->second.telemetry;
    if (current.boot_id() == record.boot_id() && current.sequence() >= record.sequence()) return;
    if (current.boot_id() != record.boot_id() && current.observed_unix_ms() > record.observed_unix_ms() + 60000) return;
  }
  _records[node] = Received{std::move(record), _time->now()};
}

void TelemetryStore::apply_identity_reset(const membership::IdentityReset& reset) {
  if (reset.host.empty() || !reset.epoch) return;
  contract::Lock lock(_mutex);
  const auto key = reset.key();
  if (const auto held = _resets.find(key); held != _resets.end() && held->second.epoch >= reset.epoch) return;
  _resets[key] = reset;
  std::erase_if(_records, [&](const auto& item) {
    const auto& t = item.second.telemetry;
    return reset.matches_endpoint(t.host(), t.port()) && reset.matches_node(item.first);
  });
}

std::optional<Telemetry> TelemetryStore::local() const {
  contract::Lock lock(_mutex);
  const auto found = _records.find(_self);
  if (found == _records.end()) return std::nullopt;
  return found->second.telemetry;
}

std::vector<Telemetry> TelemetryStore::all() const {
  contract::Lock lock(_mutex);
  std::vector<Telemetry> out;
  for (const auto& [_, received] : _records) out.push_back(received.telemetry);
  return out;
}

std::vector<Telemetry> TelemetryStore::recent(std::chrono::milliseconds max_age, size_t max_records) const {
  contract::Lock lock(_mutex);
  const auto now = _time->now();
  std::vector<Telemetry> out;
  for (const auto& [_, received] : _records)
    if (now - received.at <= max_age) out.push_back(received.telemetry);
  std::sort(out.begin(), out.end(), [](const Telemetry& a, const Telemetry& b) { return a.observed_unix_ms() > b.observed_unix_ms(); });
  if (out.size() > max_records) out.resize(max_records);
  return out;
}

std::vector<TelemetryView> TelemetryStore::views(std::chrono::milliseconds fresh_for) const {
  contract::Lock lock(_mutex);
  const auto now = _time->now();
  std::vector<TelemetryView> out;
  for (const auto& [_, received] : _records) {
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - received.at);
    out.push_back({received.telemetry, age, age <= fresh_for});
  }
  return out;
}

std::vector<Telemetry> TelemetryStore::persisted() const {
  contract::Lock lock(_mutex);
  std::vector<Telemetry> out;
  for (const auto& [_, record] : _persisted) out.push_back(record);
  return out;
}

void TelemetryStore::persist() {
  if (_path.empty()) return;
  std::map<identity::NodeId, Telemetry> merged;
  {
    contract::Lock lock(_mutex);
    merged = _persisted;
    for (const auto& [node, received] : _records) {
      const auto held = merged.find(node);
      if (held == merged.end() || held->second.observed_unix_ms() < received.telemetry.observed_unix_ms()) merged[node] = received.telemetry;
    }
  }
  std::vector<Telemetry> records;
  for (auto& [_, record] : merged) records.push_back(std::move(record));
  std::sort(records.begin(), records.end(), [](const Telemetry& a, const Telemetry& b) { return a.observed_unix_ms() > b.observed_unix_ms(); });
  if (records.size() > max_persisted) records.resize(max_persisted);
  const auto encoded = encode_telemetry_set(records);
  identity::durable_replace_file(_path, std::string_view(reinterpret_cast<const char*>(encoded.data()), encoded.size()));
  contract::Lock lock(_mutex);
  _persisted.clear();
  for (auto& record : records) _persisted[record.node_id()] = std::move(record);
}

}  // namespace dannan::status
