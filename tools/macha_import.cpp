//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "tools/macha_import.h"

#include <array>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include "dannan/identity/durable_file.h"
#include "dannan/identity/node_identity.h"
#include "dannan/membership/membership.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/codec.h"
#include "dannan/status/telemetry.h"

namespace dannan::tools {
namespace {

using Bytes = net::Bytes;

constexpr uint32_t max_nodes = 65536;
constexpr uint32_t max_resets = 65536;
constexpr size_t max_string = 4096;
constexpr size_t max_file = 16 * 1024 * 1024;
constexpr size_t max_records = 1024;

// Macha's node_flag_* bits.
constexpr uint8_t flag_inbound = 1;
constexpr uint8_t flag_hosts_extents = 2;
constexpr uint8_t flags_absent = flag_inbound | flag_hosts_extents;

// Dagda names it; a node that hosts extents carries it, whatever its value.
constexpr const char* hosts_extents_advert = "dagda/extents";

void put_magic(net::Writer& writer, std::string_view magic) { writer.raw(std::span(reinterpret_cast<const uint8_t*>(magic.data()), magic.size())); }

std::string text_of(std::span<const uint8_t> bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

std::optional<Bytes> read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return std::nullopt;
  return Bytes((std::istreambuf_iterator<char>(input)), {});
}

bool starts_with(const Bytes& bytes, std::string_view prefix) {
  return bytes.size() >= prefix.size() && std::string_view(reinterpret_cast<const char*>(bytes.data()), prefix.size()) == prefix;
}

Bytes convert_roster(const Bytes& bytes, const identity::NodeId& self, uint64_t now_unix_ms) {
  if (bytes.size() > max_file) throw net::DecodeError("the roster is too large");
  net::Reader reader(bytes);
  const auto magic = reader.fixed<8>();
  const std::string_view name(reinterpret_cast<const char*>(magic.data()), magic.size());
  int version = 0;
  if (name == "MACHMEM1") version = 1;
  if (name == "MACHMEM2") version = 2;
  if (name == "MACHMEM3") version = 3;
  if (name == "MACHMEM4") version = 4;
  if (!version) throw net::DecodeError("bad known-node roster magic");
  const auto count = reader.u32();
  if (count > max_nodes) throw net::DecodeError("too many known nodes");
  std::map<identity::NodeId, std::pair<membership::NodeRecord, uint64_t>> nodes;
  for (uint32_t i = 0; i < count; ++i) {
    membership::NodeRecord node;
    node.id.bytes = reader.fixed<16>();
    node.host = reader.string(max_string);
    node.failure_domain = reader.string(max_string);
    node.port = reader.u16();
    if (version >= 2) node.seen_unix_ms = reader.u64();
    const uint8_t flags = version >= 3 ? reader.u8() : flags_absent;
    const uint64_t last_heard = version >= 4 ? reader.u64() : now_unix_ms;
    if (node.id == identity::NodeId{} || node.id == self || node.host.empty() || !node.port) throw net::DecodeError("bad known-node roster entry");
    node.capabilities = (flags & flag_inbound) ? membership::capability_inbound : 0U;
    if (flags & flag_hosts_extents) node.adverts[hosts_extents_advert] = Bytes{1};
    const auto id = node.id;
    if (!nodes.try_emplace(id, std::move(node), last_heard).second) throw net::DecodeError("duplicate known-node roster entry");
  }
  // The reset layout is the same in both: carried through as read.
  Bytes resets;
  uint32_t reset_count = 0;
  if (version >= 2) {
    reset_count = reader.u32();
    if (reset_count > max_resets) throw net::DecodeError("too many identity association resets");
    net::Writer out;
    for (uint32_t i = 0; i < reset_count; ++i) {
      const auto host = reader.string(max_string);
      const auto port = reader.u16();
      const auto stale = reader.fixed<16>();
      const auto epoch = reader.u64();
      const auto when = reader.u64();
      const auto by = reader.fixed<16>();
      const auto reason = reader.string(max_string);
      if (host.empty() || !epoch) throw net::DecodeError("bad identity association reset");
      out.string(host);
      out.u16(port);
      out.raw(stale);
      out.u64(epoch);
      out.u64(when);
      out.raw(by);
      out.string(reason);
    }
    resets = out.take();
  }
  reader.finish();
  net::Writer writer;
  put_magic(writer, "DNNMEM01");
  writer.u32(count);
  for (const auto& [_, entry] : nodes) {
    membership::encode(writer, entry.first);
    writer.u64(entry.second);
  }
  writer.u32(reset_count);
  writer.raw(resets);
  return writer.take();
}

Bytes convert_inbound(const Bytes& bytes) {
  net::Reader reader(bytes);
  const auto magic = reader.fixed<8>();
  if (text_of(magic) != "MACHINB1") throw net::DecodeError("bad inbound resolution magic");
  const auto capable = reader.u8();
  const auto decided = reader.u64();
  const auto source = reader.string(max_string);
  reader.finish();
  net::Writer writer;
  put_magic(writer, "DNNINB01");
  writer.u8(capable ? 1 : 0);
  writer.u64(decided);
  writer.string(source);
  return writer.take();
}

// Macha's field numbers to Dannan's: the core where it has them, otherwise the
// hosted table the Macha session adopted, from status::field::first_hosted.
std::optional<uint16_t> field_for(uint16_t macha) {
  namespace field = status::field;
  if (macha >= 1 && macha <= 8) return macha;
  switch (macha) {
    case 14:
      return field::uptime_ms;
    case 15:
      return field::rss_bytes;
    case 16:
      return field::process_cpu_milli_percent;
    case 17:
      return field::load1_milli;
    case 19:
      return field::peers_known;
    case 20:
      return field::peers_active;
    case 21:
      return field::connections_created;
    case 22:
      return field::connections_reused;
    case 23:
      return field::connections_canonical;
    case 24:
      return field::phase;
    case 26:
      return field::cpu_cores;
    case 27:
      return field::memory_total_bytes;
    case 43:
      return field::node_name;
    case 44:
      return field::traffic;
    default:
      break;
  }
  static constexpr std::array<uint16_t, 22> hosted{9, 10, 11, 12, 13, 18, 25, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42};
  for (size_t i = 0; i < hosted.size(); ++i)
    if (hosted[i] == macha) return static_cast<uint16_t>(field::first_hosted + i);
  return std::nullopt;
}

Bytes convert_telemetry(const Bytes& bytes, std::string& detail) {
  if (bytes.size() > max_file) throw net::DecodeError("persisted telemetry is too large");
  net::Reader reader(bytes);
  const auto magic = reader.fixed<8>();
  if (text_of(magic) != "MACHTEL3") throw net::DecodeError("bad telemetry magic");
  const auto count = reader.u32();
  if (count > max_records) throw net::DecodeError("too many telemetry records");
  std::vector<status::Telemetry> records;
  size_t dropped = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto body = reader.bytes(max_file);
    net::Reader fields(body);
    status::Telemetry record;
    while (fields.remaining()) {
      const auto id = fields.u16();
      const auto size = fields.u16();
      const auto value = fields.raw(size);
      if (const auto mapped = field_for(id))
        record.set_bytes(*mapped, value);
      else
        ++dropped;
    }
    records.push_back(std::move(record));
  }
  reader.finish();
  const auto out = status::encode_telemetry_set(records);
  // What Dannan's loader refuses, a node would ignore: find out here.
  (void)status::decode_telemetry_set(out);
  detail = std::to_string(records.size()) + " record(s)" + (dropped ? ", " + std::to_string(dropped) + " unknown field(s) dropped" : "");
  return out;
}

struct Change {
  std::filesystem::path path;
  Bytes original;
  Bytes replacement;
};

}  // namespace

ImportReport import_macha_state(const std::filesystem::path& state, const ImportOptions& options) {
  if (!std::filesystem::is_directory(state)) throw ImportRefused("no state directory at " + state.string());
  std::optional<identity::StateLock> macha_lock, dannan_lock;
  try {
    macha_lock.emplace(state, ".macha.lock");
    dannan_lock.emplace(state, ".dannan.lock");
  } catch (const std::runtime_error& error) {
    throw ImportRefused(std::string("a node is running on this state: ") + error.what());
  }
  if (!std::filesystem::exists(state / "node-id")) throw ImportRefused("no node-id in " + state.string() + ": not a node's state");
  identity::NodeId self;
  try {
    self = identity::load_or_create_node_id(state);
  } catch (const std::exception& error) {
    throw ImportRefused(error.what());
  }
  const auto now =
      options.now_unix_ms
          ? options.now_unix_ms()
          : static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

  ImportReport report;
  std::vector<Change> changes;
  const auto consider = [&](const char* relative, const char* ours, bool must_read, const std::function<Bytes(const Bytes&, std::string&)>& convert) {
    const auto path = state / relative;
    ImportedFile file{relative, {}, {}};
    const auto bytes = read_file(path);
    if (!bytes) {
      file.outcome = "absent";
    } else if (starts_with(*bytes, ours)) {
      file.outcome = "already";
    } else {
      try {
        auto replacement = convert(*bytes, file.detail);
        changes.push_back({path, *bytes, std::move(replacement)});
        file.outcome = "imported";
      } catch (const std::exception& error) {
        if (must_read) throw ImportRefused(std::string(relative) + ": " + error.what());
        file.outcome = "ignored";
        file.detail = error.what();
      }
    }
    report.files.push_back(std::move(file));
  };
  consider("membership/known-nodes.bin", "DNNMEM01", true, [&](const Bytes& bytes, std::string& detail) {
    auto converted = convert_roster(bytes, self, now);
    // Dannan's own loader has the last word, before anything changes.
    const auto check = state / ".known-nodes.import-check";
    identity::durable_replace_file(check, text_of(converted));
    try {
      membership::NodeRecord local;
      local.id = self;
      const membership::Membership loaded(local, std::chrono::seconds(30), check);
      detail = std::to_string(loaded.all().size() - 1) + " node(s), " + std::to_string(loaded.identity_resets().size()) + " reset(s)";
    } catch (...) {
      std::filesystem::remove(check);
      throw;
    }
    std::filesystem::remove(check);
    return converted;
  });
  consider("connectivity/inbound.bin", "DNNINB01", false, [](const Bytes& bytes, std::string&) { return convert_inbound(bytes); });
  consider("telemetry/last-known.bin", "DNNTEL01", false, convert_telemetry);
  if (options.dry_run) return report;
  for (const auto& change : changes) {
    auto kept = change.path;
    kept += ".macha";
    identity::durable_replace_file(kept, text_of(change.original));
    identity::durable_replace_file(change.path, text_of(change.replacement));
  }
  return report;
}

}  // namespace dannan::tools
