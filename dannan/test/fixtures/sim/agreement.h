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

  void load(agree::Slot slot, std::span<const agree::Name> names, bool every, bool values, std::function<void(agree::Held)> done) override {
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
    if (!values)
      for (auto& state : out.states) state.value.reset();
    // The broken acceptor: each promise read back as no more than what it accepted, so the rule
    // passes ballots below a promise it gave.
    if (forget_promises)
      for (auto& state : out.states) state.promised = state.accepted;
    done(std::move(out));
  }
  // This store's delta is bytes appended to the value; every delta is checked before any is
  // applied, so a keep is all or nothing.
  void keep(agree::Slot slot, const agree::Held& held, std::function<void(Kept)> done) override {
    Kept out;
    for (size_t i = 0; i < held.names.size(); ++i) {
      if (!held.states[i].delta) continue;
      const auto found = _current.names.find({slot, text(held.names[i])});
      const auto at = found == _current.names.end() ? agree::Ballot{} : found->second.accepted;
      if (at != held.states[i].base || refuse_deltas) out.need_whole.push_back(i);
    }
    if (!out.need_whole.empty()) return done(std::move(out));
    keep_in(_current, slot, held, merging);
    if (_persist && !lying_sync) keep_in(_durable, slot, held, merging);
    ++_changes;
    out.kept = true;
    done(std::move(out));
  }
  // The store's own check refusing every delta, as a store does for one it cannot apply.
  bool refuse_deltas = false;
  bool ready() const override { return _ready; }
  void records(std::function<void(std::vector<std::pair<agree::Slot, agree::SlotState>>)> done) override {
    done(std::vector<std::pair<agree::Slot, agree::SlotState>>(_current.slots.begin(), _current.slots.end()));
  }

  // A sync: what is held is now durable (a store that does not persist each keep, `decided`).
  // One whose sync lies keeps nothing.
  void sync() {
    if (lying_sync) return;
    _durable = _current;
  }
  // A crash: what was not durable is gone.
  void crash() {
    _current = _durable;
    ++_changes;
  }
  // A restart that lost the disk too.
  void wipe() {
    _current = {};
    _durable = {};
    ++_changes;
  }

  // A promise loaded as no more than the ballot accepted: an acceptor that accepts below its
  // promise (a broken protocol a checker must catch).
  bool forget_promises = false;

  // The merge class (Mimir's): a value is a set of bytes, sorted, each once. A whole value kept
  // is merged into the one held, never replacing it, and merge() adds a byte outside agreement,
  // synced at once, leaving every ballot as it is.
  bool merging = false;
  void merge(agree::Slot slot, const agree::Name& name, uint8_t element) {
    for (auto* layer : {&_current, &_durable}) {
      auto& state = layer->names[{slot, text(name)}];
      state.value = united(state.value.value_or(agree::Bytes{}), agree::Bytes{element});
      state.has_value = true;
      state.summary = summary_of(*state.value, true);
    }
    ++_changes;
  }
  // Two sets of bytes as one.
  static agree::Bytes united(agree::Bytes a, const agree::Bytes& b) {
    a.insert(a.end(), b.begin(), b.end());
    std::sort(a.begin(), a.end());
    a.erase(std::unique(a.begin(), a.end()), a.end());
    return a;
  }

  // For a checker: a count that moves whenever what is held may have, every name's state, every
  // slot's record, and the floor.
  uint64_t changes() const noexcept { return _changes; }
  template <class Visit>
  void each_name(Visit&& visit) const {
    for (const auto& [key, state] : _current.names) visit(key.first, key.second, state);
  }
  const std::map<agree::Slot, agree::SlotState>& slots_held() const noexcept { return _current.slots; }
  const agree::Ballot& floor() const noexcept { return _current.floor; }
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
  static void keep_in(Layer& layer, agree::Slot slot, const agree::Held& held, bool merging) {
    layer.floor = held.floor;
    layer.slots[slot] = held.slot;
    for (size_t i = 0; i < held.names.size(); ++i) {
      const std::pair<agree::Slot, std::string> key{slot, text(held.names[i])};
      auto state = held.states[i];
      if (state == agree::AcceptorState{}) {
        layer.names.erase(key);
        continue;
      }
      if (state.delta) {
        // Applied in place to the value at the base, and summarised.
        const auto before = layer.names.find(key);
        auto value = before != layer.names.end() && before->second.value ? *before->second.value : agree::Bytes{};
        value.insert(value.end(), state.delta->begin(), state.delta->end());
        state.value = std::move(value);
        state.summary = summary_of(*state.value, merging);
        state.delta.reset();
        state.base = {};
      } else if (state.has_value && !state.value) {
        // Held, not carried: the value and its summary stay as they are.
        const auto before = layer.names.find(key);
        if (before != layer.names.end()) {
          state.value = before->second.value;
          state.summary = before->second.summary;
        }
      } else {
        if (merging && state.value) {
          const auto before = layer.names.find(key);
          if (before != layer.names.end() && before->second.value) state.value = united(*state.value, *before->second.value);
        }
        state.has_value = state.value.has_value();
        state.summary = state.value ? summary_of(*state.value, merging) : agree::Bytes{};
      }
      layer.names[key] = std::move(state);
    }
  }
  // This store's summary: the value's length, eight bytes big-endian. The merge class's is a
  // digest of the content (FNV-1a 64), as Mimir's is: two merged states of one length differ,
  // and a proposer fetching by ballot and summary must not take one for the other.
  static agree::Bytes summary_of(const agree::Bytes& value, bool merging = false) {
    uint64_t word = value.size();
    if (merging) {
      word = 0xcbf29ce484222325ull;
      for (const auto byte : value) word = (word ^ byte) * 0x100000001b3ull;
    }
    agree::Bytes out(8);
    for (int i = 0; i < 8; ++i) out[static_cast<size_t>(i)] = static_cast<uint8_t>(word >> (56 - 8 * i));
    return out;
  }

 public:
  // The summary this store gives a value, for a test's checks.
  static agree::Bytes summary(const agree::Bytes& value) { return summary_of(value); }

 private:
  bool _persist;
  bool _ready = true;
  uint64_t _changes = 0;
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
  // Each link keeps its order: a message is delayed as above but never arrives before one sent
  // earlier on the same link, as on one session. Off: they overtake (the default). Whole nodes'
  // network (SimNetwork) keeps it.
  bool in_order = false;
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

  // Every answer that crossed the world while `recording`: who asked whom, the request's type,
  // and the answer's bytes. A node's own acceptor answers it without the world.
  struct Crossed {
    size_t from = 0;
    size_t to = 0;
    net::MessageType type{};
    net::Bytes answer;
    size_t request_bytes = 0;
  };
  bool recording = false;
  std::vector<Crossed> crossed;

  // Told of every restart that lost its state, before the node starts again: a checker of
  // the acceptors' state starts that acceptor afresh. A loss it is not told of (a lying sync)
  // is one it must catch.
  std::function<void(size_t)> on_wipe;

  // A request from node `from` to node `to` through the world, as the runtime's own calls go:
  // for a test's own proposer (a broken one).
  void call(size_t from, size_t to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline,
            std::function<void(std::optional<net::Bytes>, std::string)> done) {
    send(from, to, type, std::move(payload), deadline, std::move(done));
  }

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
    if (wiped) {
      _nodes.at(i)->store->wipe();
      if (on_wipe) on_wipe(i);
    }
    _nodes.at(i)->store->set_ready(ready);
    start(i);
  }
  // The node starts again configured with another slot count (a misconfigured node).
  void restart_with_slots(size_t i, uint32_t count) {
    crash(i);
    _nodes.at(i)->slots = count;
    start(i);
  }

 private:
  struct Node {
    identity::NodeId id;
    std::shared_ptr<Store> store;
    std::unique_ptr<agree::Agreement> agreement;
    uint32_t slots = Cluster::slots;
  };

  void start(size_t i) {
    auto& node = *_nodes.at(i);
    identity::NodeId epoch;
    for (auto& byte : epoch.bytes) byte = static_cast<uint8_t>(_world.random.next());
    node.agreement = std::make_unique<agree::Agreement>(
        _world.io.get_executor(), node.id, epoch, agree::Agreement::Table{node.slots, _genesis},
        [this, i](const identity::NodeId& to, net::MessageType type, net::Bytes payload, std::chrono::milliseconds deadline,
                  std::function<void(std::optional<net::Bytes>, std::string)> done) {
          send(i, index(to), type, std::move(payload), deadline, std::move(done));
        },
        [](const identity::NodeId&, const std::string&) { return true; }, &_time,
        [this](std::chrono::milliseconds after, std::function<void()> task) { _world.after(static_cast<uint64_t>(after.count()), std::move(task)); },
        // Adverts reach every node at once: the world's gossip is not what is under test.
        [this, i](const std::string& key, std::optional<net::Bytes> value) {
          if (value)
            _adverts[{i, key}] = std::move(*value);
          else
            _adverts.erase({i, key});
        },
        [this](const std::string& key) {
          std::vector<std::pair<identity::NodeId, net::Bytes>> out;
          for (const auto& [at, value] : _adverts)
            if (at.second == key) out.emplace_back(id(at.first), value);
          return out;
        });
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
        const auto answer = [this, from, to, type, size = payload.size(), finish](net::Message reply) {
          if (lost(to, from)) return;
          if (recording) crossed.push_back({from, to, type, reply.payload, size});
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
  std::map<std::pair<size_t, std::string>, net::Bytes> _adverts;
  std::vector<std::unique_ptr<Node>> _nodes;
  std::set<std::pair<size_t, size_t>> _blocked;
};

}  // namespace dannan::test::sim
