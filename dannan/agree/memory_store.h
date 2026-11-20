//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dannan/agree/agreement.h"
#include "dannan/contract/thread_safety.h"

// An acceptor store held in memory (TODO/2026-11-20-stack-plan.md, item 1), for a hosted
// system's spaces before a durable store holds them (Dagda's), and for tests. It answers from
// memory, so a write it accepts is `decided`, never `replicated`.
//
// A process that starts holds nothing it voted before. Unless it is told it holds everything
// there is to hold (a cluster's first start), it is not ready: it may not vote until it has
// caught up and the hosted system calls set_ready(). A store that voted with lost state could
// commit two values for one name.
//
// Values are the hosted system's bytes. The codec says how they are summarised (SHA-256 by
// default) and how a delta is applied to the value at its base; with no apply, every delta is
// asked for whole. keep() is all or nothing: every delta and summary is checked before anything
// changes. Every completion is posted to the executor the store was made with, never called
// inline. Thread-safe.
namespace dannan::agree {

class MemoryStore final : public AcceptorStore {
 public:
  struct Codec {
    // The owner's summary of a value, at most max_summary bytes; a longer one refuses the keep.
    // Empty: the value's SHA-256.
    std::function<Bytes(const Bytes& value)> summary;
    // `value` with `delta` applied, or nothing if this delta cannot be applied to it (asked for
    // whole). Empty: every delta is asked for whole.
    std::function<std::optional<Bytes>(const Bytes& value, const Bytes& delta)> apply;
  };

  // `ready`: whether it may vote at once, true only when nothing was held before.
  MemoryStore(boost::asio::any_io_executor executor, bool ready, Codec codec = {});

  void load(Slot slot, std::span<const Name> names, bool every, bool values, std::function<void(Held)> done) override;
  void keep(Slot slot, const Held& held, std::function<void(Kept)> done) override;
  bool ready() const override { return _ready.load(); }
  void records(std::function<void(std::vector<std::pair<Slot, SlotState>>)> done) override;

  // Caught up: it may vote from now on.
  void set_ready() { _ready.store(true); }

  // What this acceptor holds for `name`, for tests and Status.
  std::optional<AcceptorState> state(Slot slot, const Name& name) const;
  // How many names it holds state for, every slot together.
  size_t size() const;

 private:
  struct SlotHeld {
    SlotState record;
    // Keyed by the name's bytes as a string.
    std::map<std::string, AcceptorState> names;
  };

  static std::string key_of(std::span<const uint8_t> name) { return {reinterpret_cast<const char*>(name.data()), name.size()}; }
  Bytes summary_of(const Bytes& value) const;
  // What `held` keeps for each name, or nothing with the reason: need_whole's indices, or none
  // for a refusal of the whole keep.
  std::optional<std::vector<AcceptorState>> prepared(Slot slot, const Held& held, Kept& refusal) const DANNAN_REQUIRES(_mutex);

  boost::asio::any_io_executor _executor;
  std::atomic<bool> _ready;
  const Codec _codec;
  mutable contract::Mutex _mutex;
  std::map<Slot, SlotHeld> _slots DANNAN_GUARDED_BY(_mutex);
  Ballot _floor DANNAN_GUARDED_BY(_mutex);
};

}  // namespace dannan::agree
