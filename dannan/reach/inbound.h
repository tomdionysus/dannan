//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "dannan/membership/node_record.h"
#include "dannan/net/client.h"

namespace dannan::reach {

// Whether peers can connect to this node: configured, or decided by
// dial-back (`automatic`).
enum class Inbound : uint8_t { yes, no, automatic };

struct InboundState {
  Inbound mode = Inbound::automatic;
  bool capable = true;
  // configured, default, persisted, or probe:<peer id>.
  std::string source;
  uint64_t decided_unix_ms = 0;
  uint64_t last_probe_unix_ms = 0;
  std::string last_probe_peer;
  std::string last_probe_error;
  unsigned consecutive_failures = 0;
};

// What a dial-back's answer did, and when to ask again.
struct ProbeVerdict {
  bool changed = false;
  std::chrono::milliseconds next_probe{};
};

// The answer to "can peers connect to me?". A configured value is final.
// `automatic` starts from the persisted answer, so a restart does not look
// like a join or a leave, else as capable until a dial-back says otherwise.
// The evidence is a peer reporting whether a fresh connection to this
// node's advertised endpoint completed a handshake. Sticky: capable to
// incapable takes two failures in a row, back takes one success. Persisted
// under the state path as protocol 23's connectivity/inbound.bin.
// Thread-safe; waits on the file when it persists.
class InboundResolver {
 public:
  static constexpr std::chrono::milliseconds confirm_after{5000};

  InboundResolver(Inbound mode, std::filesystem::path state, const contract::WallClock* wall = &contract::system_wall_clock(),
                  std::chrono::milliseconds reprobe_while_capable = std::chrono::hours(1),
                  std::chrono::milliseconds reprobe_while_incapable = std::chrono::minutes(10), std::chrono::milliseconds confirm = confirm_after);

  bool capable() const;
  InboundState state() const;
  // A peer's answer to this node's dial-back request. Nothing changes for a
  // configured mode.
  ProbeVerdict observe_probe(const identity::NodeId& peer, bool reachable, const std::string& error);

 private:
  void persist(const InboundState& state);

  const std::filesystem::path _path;
  const contract::WallClock* const _wall;
  const std::chrono::milliseconds _reprobe_capable;
  const std::chrono::milliseconds _reprobe_incapable;
  const std::chrono::milliseconds _confirm;
  mutable contract::Mutex _mutex;
  InboundState _state DANNAN_GUARDED_BY(_mutex);
  // Orders writes of the file; guards nothing else.
  contract::IoMutex _persist_mutex;
};

// Refuses, with the reason in the operator's terms, a node that takes no
// inbound connection and either founds the cluster or has only bootstrap
// peers known to take none either: nothing could ever reach it.
void refuse_impossible_cluster(Inbound mode, const std::vector<net::Endpoint>& bootstrap, const std::vector<membership::NodeRecord>& known,
                               const identity::NodeId& self);

}  // namespace dannan::reach
