//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dannan/agree/agreement.h"
#include "dannan/net/message.h"
#include "dannan/test/fixtures/sim/world.h"

// Agreement inside the world: each simulated node runs Dannan's real Agreement over the
// world's network, time and schedule, with an acceptor store whose durability the world
// controls. Faults are the world's to inject: lost, delayed, duplicated and one-way-blocked
// messages, crashes that drop what was not synced, and restarts with or without state.
namespace dannan::test::sim {

// An acceptor store with a volatile and a durable layer. `persist`: each keep is durable
// before it answers (`replicated`); otherwise it answers from memory (`decided`) and only a
// sync makes it durable. A crash keeps only the durable layer.
class Store final : public agree::AcceptorStore {
 public:
  explicit Store(bool persist) : _persist(persist) {}

  void load(agree::Slot slot, std::span<const agree::Name> names, bool every, std::function<void(agree::Held)> done) override {
    agree::Held out;
    out.floor = _current.floor;
    const auto record = _current.slots.find(slot);
    if (record != _current.slots.end()) out.slot = record->second;
    for (const auto& name : names) {
      out.names.push_back(name);
      const auto held = _current.names.find({slot, text(name)});
      out.states.push_back(held != _current.names.end() ? held->second : agree::AcceptorState{});
    }
    if (every)
      for (const auto& [key, state] : _current.names) {
        if (key.first != slot) continue;
        const agree::Name name(key.second.begin(), key.second.end());
        if (std::find(names.begin(), names.end(), name) != names.end()) continue;
        out.names.push_back(name);
        out.states.push_back(state);
      }
    done(std::move(out));
  }
  void keep(agree::Slot slot, const agree::Held& held, std::function<void(bool)> done) override {
    keep_in(_current, slot, held);
    if (_persist && !lying_sync) keep_in(_durable, slot, held);
    done(true);
  }
  bool ready() const override { return _ready; }

  // A crash: what was not durable is gone.
  void crash() { _current = _durable; }
  // A restart that lost the disk too.
  void wipe() {
    _current = {};
    _durable = {};
  }
  void set_ready(bool ready) { _ready = ready; }
  // A sync that reports success and keeps nothing (the broken store a checker must catch).
  bool lying_sync = false;

  // What this acceptor holds, for a test's checks.
  std::optional<agree::SlotState> slot(agree::Slot slot) const {
    const auto found = _current.slots.find(slot);
    return found == _current.slots.end() ? std::nullopt : std::optional(found->second);
  }
  std::optional<agree::AcceptorState> state(agree::Slot slot, const agree::Name& name) const {
    const auto found = _current.names.find({slot, text(name)});
    return found == _current.names.end() ? std::nullopt : std::optional(found->second);
  }

 private:
  // Keyed by the name's bytes as a string: GCC 14 at -O3 misreads a map of byte vectors.
  struct Layer {
    std::map<std::pair<agree::Slot, std::string>, agree::AcceptorState> names;
    std::map<agree::Slot, agree::SlotState> slots;
    agree::Ballot floor;
  };
  static std::string text(const agree::Name& name) { return std::string(name.begin(), name.end()); }
  static void keep_in(Layer& layer, agree::Slot slot, const agree::Held& held) {
    layer.floor = held.floor;
    layer.slots[slot] = held.slot;
    for (size_t i = 0; i < held.names.size(); ++i) {
      if (held.states[i] == agree::AcceptorState{})
        layer.names.erase({slot, text(held.names[i])});
      else
        layer.names[{slot, text(held.names[i])}] = held.states[i];
    }
  }
  bool _persist;
  bool _ready = true;
  Layer _current;
  Layer _durable;
};

// How the network misbehaves, each drawn from the world's seed.
struct NetworkFaults {
  // One message in this many is lost; zero: none.
  uint64_t drop_one_in = 0;
  // One request in this many is delivered twice.
  uint64_t duplicate_one_in = 0;
  // Each message takes 1 to this many milliseconds, so messages overtake each other.
  uint64_t max_delay_ms = 20;
};

class Cluster {
 public:
  static constexpr const char* space = "sim";
  // The cluster's slot table: small, so founding it is cheap.
  static constexpr uint32_t slots = 4;
  // The genesis set's version.
  static constexpr uint64_t genesis_version = 0;

  // `size` nodes, each serving the space from a store that persists before answering if
  // `persist`. The first `genesis` of them (all, if zero) are the genesis set.
  Cluster(World& world, size_t size, NetworkFaults faults, bool persist = true, size_t genesis = 0) : _world(world), _time(world), _faults(faults) {
    for (size_t i = 0; i < size; ++i) {
      auto node = std::make_unique<Node>();
      node->id.bytes[15] = static_cast<uint8_t>(i + 1);
      node->store = std::make_shared<Store>(persist);
      _nodes.push_back(std::move(node));
    }
    std::vector<identity::NodeId> founders;
    for (size_t i = 0; i < (genesis ? genesis : size); ++i) founders.push_back(_nodes[i]->id);
    _genesis = agree::ReplicaSet::of(std::move(founders), genesis_version);
    for (size_t i = 0; i < size; ++i) start(i);
  }
  Cluster(const Cluster&) = delete;
  Cluster& operator=(const Cluster&) = delete;

  size_t size() const noexcept { return _nodes.size(); }
  const identity::NodeId& id(size_t i) const { return _nodes.at(i)->id; }
  std::vector<identity::NodeId> ids() const {
    std::vector<identity::NodeId> out;
    for (const auto& node : _nodes) out.push_back(node->id);
    return out;
  }
  const agree::ReplicaSet& genesis() const noexcept { return _genesis; }
  // The ids of the nodes at `indices`.
  std::vector<identity::NodeId> ids(std::initializer_list<size_t> indices) const {
    std::vector<identity::NodeId> out;
    for (const auto i : indices) out.push_back(_nodes.at(i)->id);
    return out;
  }
  bool up(size_t i) const { return _nodes.at(i)->agreement != nullptr; }
  agree::Agreement& agreement(size_t i) { return *_nodes.at(i)->agreement; }
  Store& store(size_t i) { return *_nodes.at(i)->store; }
  NetworkFaults& faults() { return _faults; }

  // Messages from `from` to `to` are lost until unblocked: a one-way partition.
  void block(size_t from, size_t to) { _blocked.insert({from, to}); }
  void unblock_all() { _blocked.clear(); }

  // The node stops: its in-flight changes end, and its store keeps only what was durable.
  void crash(size_t i) {
    _nodes.at(i)->agreement.reset();
    _nodes.at(i)->store->crash();
  }
  // The node starts again under a new epoch; without its state if `wiped`. A store that
  // lost its state must not vote until caught up: `ready` says whether it does anyway
  // (the broken protocol a checker must catch).
  void restart(size_t i, bool wiped = false, bool ready = true) {
    if (wiped) _nodes.at(i)->store->wipe();
    _nodes.at(i)->store->set_ready(ready);
    start(i);
  }

 private:
  struct Node {
    identity::NodeId id;
    std::shared_ptr<Store> store;
    std::unique_ptr<agree::Agreement> agreement;
  };

  void start(size_t i) {
    auto& node = *_nodes.at(i);
    identity::NodeId epoch;
    for (auto& byte : epoch.bytes) byte = static_cast<uint8_t>(_world.random.next());
    node.agreement = std::make_unique<agree::Agreement>(
        _world.io.get_executor(), node.id, epoch, agree::Agreement::Table{slots, _genesis},
        [this, i](const identity::NodeId& to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline,
                  std::function<void(std::optional<net::Bytes>, std::string)> done) {
          send(i, index(to), type, std::move(payload), deadline, std::move(done));
        },
        [](const identity::NodeId&, const std::string&) { return true; }, &_time,
        [this](std::chrono::milliseconds after, std::function<void()> task) { _world.after(static_cast<uint64_t>(after.count()), std::move(task)); });
    node.agreement->serve(space, space, node.store);
  }

  size_t index(const identity::NodeId& id) const {
    for (size_t i = 0; i < _nodes.size(); ++i)
      if (_nodes[i]->id == id) return i;
    return _nodes.size();
  }

  uint64_t delay() { return 1 + _world.random.below(_faults.max_delay_ms); }
  bool lost(size_t from, size_t to) { return _blocked.contains({from, to}) || _world.random.one_in(_faults.drop_one_in); }

  // A call through the world: delivered after a delay unless lost, perhaps twice; its answer
  // comes back the same way; told once, by the answer or by its deadline.
  void send(size_t from, size_t to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline,
            std::function<void(std::optional<net::Bytes>, std::string)> done) {
    auto told = std::make_shared<bool>(false);
    auto finish = [told, done = std::move(done)](std::optional<net::Bytes> reply, std::string why) {
      if (*told) return;
      *told = true;
      done(std::move(reply), std::move(why));
    };
    _world.after(static_cast<uint64_t>(deadline.count()), [finish] { finish(std::nullopt, "no answer within the deadline"); });
    if (to >= _nodes.size()) return;
    const int copies = _world.random.one_in(_faults.duplicate_one_in) ? 2 : 1;
    for (int copy = 0; copy < copies; ++copy) {
      if (lost(from, to)) continue;
      _world.after(delay(), [this, from, to, type, payload, finish] {
        if (!up(to)) return;
        const auto answer = [this, from, to, finish](net::Message reply) {
          if (lost(to, from)) return;
          _world.after(delay(), [finish, reply = std::move(reply)] {
            if (const auto refusal = net::refusal_of(reply)) return finish(std::nullopt, refusal->message);
            finish(reply.payload, {});
          });
        };
        if (auto now = agreement(to).serve_request(id(from), net::Message{type, payload, {}}, answer)) answer(std::move(*now));
      });
    }
  }

  World& _world;
  Time _time;
  NetworkFaults _faults;
  agree::ReplicaSet _genesis;
  std::vector<std::unique_ptr<Node>> _nodes;
  std::set<std::pair<size_t, size_t>> _blocked;
};

}  // namespace dannan::test::sim
