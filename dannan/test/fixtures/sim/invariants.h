//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "dannan/test/fixtures/sim/agreement.h"
#include "dannan/test/fixtures/sim/world.h"

// Agreement's own invariants (the harness design's "History and checkers"), checked over every
// acceptor's state after every step of the world:
//  1. at most one value is accepted at a ballot, for a name;
//  2. a value once chosen (accepted by a majority of the set its acceptors record as serving
//     the slot) is the base of every later one: each value accepted at a higher ballot
//     advances it, by the owner's order;
//  3. changed_at, the fencing token, never goes back from one chosen value to a later one;
//  4. no acceptor promises, or accepts, below a promise it holds.
// A restart without its state is a fault, not a breach: the cluster tells the checker, and that
// acceptor starts afresh. A loss it is not told of (a sync that lied) is caught.
namespace dannan::test::sim {

class Invariants {
 public:
  // Whether `later` is `earlier` advanced by zero or more of the owner's operations.
  using Advances = std::function<bool(const std::optional<agree::Bytes>& earlier, const std::optional<agree::Bytes>& later)>;
  struct Options {
    // Empty: invariant 2 is not checked.
    Advances advances;
    // Names written outside agreement too (Mimir's merge class): acceptors at one ballot hold
    // different merged states by design, so invariant 1 is not checked, and a chosen value is
    // what every acceptor of its ballot held in common.
    bool merge_class = false;
  };

  // Watches `cluster` on every step of `world`, and is told of its wipes, until destroyed.
  Invariants(World& world, Cluster& cluster, Options options)
      : _world(world), _cluster(cluster), _options(std::move(options)), _acceptors(cluster.size()), _looked(cluster.size(), never) {
    _world.observer = [this] { check(); };
    _cluster.on_wipe = [this](size_t acceptor) { lost(acceptor); };
  }
  ~Invariants() {
    _world.observer = {};
    _cluster.on_wipe = {};
  }
  Invariants(const Invariants&) = delete;
  Invariants& operator=(const Invariants&) = delete;

  // Looks at every acceptor whose store changed since it last looked. After the first breach,
  // nothing more: the first is the one to read.
  void check() {
    for (size_t i = 0; i < _cluster.size() && !_violation; ++i) {
      const auto& store = _cluster.store(i);
      if (store.changes() == _looked[i]) continue;
      _looked[i] = store.changes();
      look(i, store);
    }
  }

  // Acceptor `i` restarted without its state: what it held before binds it no longer.
  void lost(size_t i) {
    _acceptors.at(i) = {};
    _looked.at(i) = never;
  }

  // The first breach: when, which acceptor and name, and what.
  const std::optional<std::string>& violation() const noexcept { return _violation; }
  // How many ballots were seen chosen: what invariants 2 and 3 had to work on.
  size_t chosen() const noexcept { return _chosen; }

 private:
  static constexpr uint64_t never = std::numeric_limits<uint64_t>::max();
  using Key = std::pair<agree::Slot, std::string>;
  struct Promised {
    agree::Ballot promised;
    agree::Ballot accepted;
  };
  struct Acceptor {
    std::map<Key, Promised> names;
    std::map<agree::Slot, agree::Ballot> slots;
  };
  // One ballot of one name, as the acceptors that accepted it held it.
  struct Accepted {
    std::optional<agree::Bytes> value;
    agree::Ballot changed_at;
    std::optional<agree::ReplicaSet> set;
    std::set<size_t> acceptors;
    bool chosen = false;
  };

  static std::string text(const agree::Ballot& ballot) { return std::to_string(ballot.counter) + "/" + std::to_string(ballot.node.bytes[15]); }
  static std::string text(const Key& key) {
    std::string name;
    for (const auto byte : key.second) name += byte >= 0x20 && byte < 0x7f ? static_cast<char>(byte) : '?';
    return "slot " + std::to_string(key.first) + " name '" + name + "'";
  }

  void fail(size_t acceptor, const std::string& what) {
    if (!_violation) _violation = "at " + std::to_string(_world.now_ms()) + " ms, acceptor " + std::to_string(acceptor) + ": " + what;
  }

  void look(size_t i, const Store& store) {
    auto& acceptor = _acceptors[i];
    // Invariant 4 for a slot's transfer promise.
    for (const auto& [slot, record] : store.slots_held()) {
      auto& last = acceptor.slots[slot];
      if (record.promised < last)
        return fail(i, "slot " + std::to_string(slot) + " promised " + text(record.promised) + ", below the promise " + text(last) + " it held");
      last = record.promised;
    }
    for (const auto& [slot, last] : acceptor.slots)
      if (!store.slots_held().contains(slot) && !last.zero()) return fail(i, "slot " + std::to_string(slot) + " forgot the promise " + text(last) + " it held");

    std::set<Key> present;
    store.each_name([&](agree::Slot slot, const std::string& name, const agree::AcceptorState& state) {
      if (_violation) return;
      const Key key{slot, name};
      present.insert(key);
      auto& last = acceptor.names[key];
      // Invariant 4: a promise only rises, and an accept is at or above the promise held.
      if (state.promised < last.promised)
        return fail(i, text(key) + " promised " + text(state.promised) + ", below the promise " + text(last.promised) + " it held");
      if (state.accepted != last.accepted) {
        if (state.accepted < last.promised)
          return fail(i, text(key) + " accepted " + text(state.accepted) + ", below the promise " + text(last.promised) + " it held");
        accepted(i, key, state, store);
      }
      last = {state.promised, state.accepted};
    });
    if (_violation) return;
    // A name gone is purged, with the floor raised past what it held, or forgotten: a breach.
    for (auto it = acceptor.names.begin(); it != acceptor.names.end();) {
      if (present.contains(it->first)) {
        ++it;
        continue;
      }
      if (store.floor() < it->second.promised)
        return fail(i, text(it->first) + " forgot the promise " + text(it->second.promised) + " it held, its floor only " + text(store.floor()));
      it = acceptor.names.erase(it);
    }
  }

  // Acceptor `i` accepted a new ballot for `key`.
  void accepted(size_t i, const Key& key, const agree::AcceptorState& state, const Store& store) {
    if (state.accepted.zero()) return;
    auto& ballots = _ballots[key];
    auto& at = ballots[state.accepted];
    if (at.acceptors.empty()) {
      at.value = state.value;
      at.changed_at = state.changed_at;
      const auto record = store.slots_held().find(key.first);
      if (record != store.slots_held().end()) at.set = record->second.served;
    } else if (_options.merge_class) {
      at.value = common(at.value, state.value);
    } else if (at.value != state.value) {
      // Invariant 1.
      return fail(i, text(key) + " accepted at " + text(state.accepted) + " a value another acceptor accepted differently at the same ballot");
    }
    at.acceptors.insert(i);
    // Invariant 2, against every value already chosen below it.
    if (_options.advances)
      for (const auto& [ballot, other] : ballots)
        if (other.chosen && ballot < state.accepted && !_options.advances(other.value, at.value))
          return fail(i, text(key) + " accepted at " + text(state.accepted) + " a value that does not advance the value chosen at " + text(ballot));
    if (at.chosen || !at.set) return;
    const auto in_set = std::count_if(at.acceptors.begin(), at.acceptors.end(), [&](size_t a) { return at.set->contains(_cluster.id(a)); });
    if (static_cast<size_t>(in_set) >= at.set->majority()) chose(i, key, state.accepted, at);
  }

  // `ballot` of `key` is chosen: every value accepted above it must advance it, and every chosen
  // ballot's fencing token is ordered as the ballots are.
  void chose(size_t i, const Key& key, const agree::Ballot& ballot, Accepted& at) {
    at.chosen = true;
    ++_chosen;
    for (const auto& [other_ballot, other] : _ballots[key]) {
      if (other_ballot == ballot) continue;
      if (other.chosen && (other_ballot < ballot ? at.changed_at < other.changed_at : other.changed_at < at.changed_at))
        return fail(i, text(key) + " chose " + text(ballot) + " with changed_at " + text(at.changed_at) + ", out of order with " + text(other_ballot) + "'s " +
                           text(other.changed_at));
      if (_options.advances && ballot < other_ballot && !_options.advances(at.value, other.value))
        return fail(i, text(key) + " chose at " + text(ballot) + " a value that the value accepted at " + text(other_ballot) + " does not advance");
    }
  }

  // What two merged states hold in common, as sorted sets of bytes.
  static std::optional<agree::Bytes> common(const std::optional<agree::Bytes>& a, const std::optional<agree::Bytes>& b) {
    if (!a || !b) return std::nullopt;
    agree::Bytes out;
    std::set_intersection(a->begin(), a->end(), b->begin(), b->end(), std::back_inserter(out));
    return out;
  }

  World& _world;
  Cluster& _cluster;
  Options _options;
  std::vector<Acceptor> _acceptors;
  std::vector<uint64_t> _looked;
  std::map<Key, std::map<agree::Ballot, Accepted>> _ballots;
  size_t _chosen = 0;
  std::optional<std::string> _violation;
};

}  // namespace dannan::test::sim
