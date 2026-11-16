//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/test/fixtures/sim/linearizable.h"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <unordered_set>
#include <utility>

namespace dannan::test::sim {
namespace {

// How an operation counts: never at all, with a known result, or with none known.
enum class Counts { never, known, unknown };

Counts counts(const Operation& operation) {
  if (operation.answer == Answer::cancelled) return Counts::never;
  if (operation.resolved && !*operation.resolved) return Counts::never;
  if (operation.answer == Answer::ok && (!operation.provisional || operation.resolved)) return Counts::known;
  return Counts::unknown;
}

// The latest instant a known operation could have taken effect: its answer, or, answered
// before the cluster decided it, the decision.
uint64_t returned(const Operation& operation) {
  if (operation.provisional && operation.resolved_ms) return std::max(*operation.resolved_ms, *operation.answered_ms);
  return *operation.answered_ms;
}

// Union-find over keys, by index.
struct Groups {
  std::vector<size_t> parent;
  size_t find(size_t at) {
    while (parent[at] != at) at = parent[at] = parent[parent[at]];
    return at;
  }
  void join(size_t a, size_t b) { parent[find(a)] = find(b); }
};

// One group's search: Wing and Gong's, with Lowe's memo of (taken, state).
class Search {
 public:
  Search(const History& history, const Model& model, std::vector<size_t> members, std::vector<std::string> keys, size_t budget)
      : _ops(history.operations()), _model(model), _members(std::move(members)), _keys(std::move(keys)), _budget(budget) {}

  // Whether the group is explained; fills `out` when not.
  bool run(Verdict& out) {
    build();
    std::string state = _model.initial(_keys);
    std::vector<uint64_t> taken((_members.size() + 63) / 64, 0);
    std::unordered_set<std::string> seen;
    struct Frame {
      size_t entry;
      std::string before;
      std::vector<std::string> next;
      size_t tried;
    };
    std::vector<Frame> stack;
    size_t deepest = 0;
    size_t stopped = 0;
    const auto key_of = [&taken](const std::string& next) {
      std::string key(reinterpret_cast<const char*>(taken.data()), taken.size() * sizeof(uint64_t));
      key.push_back('\0');
      key += next;
      return key;
    };
    // The first of `next` from `from` not seen with `member` taken; its index, or none.
    const auto untried = [&](size_t member, const std::vector<std::string>& next, size_t from) -> std::optional<size_t> {
      set(taken, member, true);
      for (size_t i = from; i < next.size(); ++i)
        if (seen.insert(key_of(next[i])).second) return i;
      set(taken, member, false);
      return std::nullopt;
    };
    size_t entry = _next[head];
    while (true) {
      if (entry == end) return true;
      if (++out.steps > _budget) {
        out.exhausted = true;
        out.why = "the search ran out of steps";
        return false;
      }
      const auto member = _member_of[entry];
      const auto& operation = _ops[_members[member]];
      if (_call[entry]) {
        // Unanswered operations alike in keys and input are taken in invocation order: any
        // order taking a later one could take the earlier instead, so nothing is lost and the
        // search stays linear in them, not exponential.
        if (_earlier[member] != none && !get(taken, _earlier[member])) {
          entry = _next[entry];
          continue;
        }
        auto next = _model.step(state, operation, counts(operation) == Counts::known ? &operation.output : nullptr);
        if (const auto chosen = untried(member, next, 0)) {
          stack.push_back(Frame{entry, state, std::move(next), *chosen});
          state = stack.back().next[*chosen];
          lift(entry);
          entry = _next[head];
        } else {
          entry = _next[entry];
        }
        continue;
      }
      // An operation's answer came before any order could take it: undo the last choice. The
      // one that stopped the deepest order found is the one named.
      if (stack.size() >= deepest) {
        deepest = stack.size();
        stopped = member;
      }
      if (stack.empty()) {
        const auto& named = _ops[_members[stopped]];
        out.unexplained.push_back(_members[stopped]);
        out.why = "no order explains operation " + std::to_string(_members[stopped]) + " (client " + std::to_string(named.client) + ")";
        return false;
      }
      auto& frame = stack.back();
      const auto undone = _member_of[frame.entry];
      unlift(frame.entry);
      set(taken, undone, false);
      state = frame.before;
      if (const auto again = untried(undone, frame.next, frame.tried + 1)) {
        frame.tried = *again;
        state = frame.next[*again];
        lift(frame.entry);
        entry = _next[head];
        continue;
      }
      entry = _next[frame.entry];
      stack.pop_back();
    }
  }

 private:
  static constexpr size_t head = 0;
  static constexpr size_t end = 1;

  static constexpr size_t none = std::numeric_limits<size_t>::max();

  static bool get(const std::vector<uint64_t>& bits, size_t at) { return (bits[at / 64] >> (at % 64)) & 1u; }
  static void set(std::vector<uint64_t>& bits, size_t at, bool on) {
    if (on)
      bits[at / 64] |= uint64_t{1} << (at % 64);
    else
      bits[at / 64] &= ~(uint64_t{1} << (at % 64));
  }

  // The entries in time order, calls before answers at one instant (so they count as
  // concurrent), linked between a head and an end.
  void build() {
    struct Event {
      uint64_t time;
      bool call;
      size_t member;
    };
    std::vector<Event> events;
    for (size_t m = 0; m < _members.size(); ++m) {
      const auto& operation = _ops[_members[m]];
      events.push_back({operation.invoked_ms, true, m});
      if (counts(operation) == Counts::known) events.push_back({returned(operation), false, m});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
      if (a.time != b.time) return a.time < b.time;
      return a.call && !b.call;
    });
    const size_t count = events.size() + 2;
    _next.assign(count, end);
    _prev.assign(count, head);
    _call.assign(count, false);
    _member_of.assign(count, 0);
    _match.assign(count, end);
    std::vector<size_t> call_of(_members.size(), end);
    size_t last = head;
    for (size_t i = 0; i < events.size(); ++i) {
      const size_t at = i + 2;
      _call[at] = events[i].call;
      _member_of[at] = events[i].member;
      if (events[i].call) {
        call_of[events[i].member] = at;
      } else {
        _match[call_of[events[i].member]] = at;
      }
      _next[last] = at;
      _prev[at] = last;
      last = at;
    }
    _next[last] = end;
    _prev[end] = last;
    // Each unanswered operation's predecessor among those alike to it, by invocation.
    _earlier.assign(_members.size(), none);
    std::map<std::pair<std::vector<std::string>, Bytes>, size_t> latest;
    std::vector<size_t> by_invocation(_members.size());
    std::iota(by_invocation.begin(), by_invocation.end(), size_t{0});
    std::stable_sort(by_invocation.begin(), by_invocation.end(),
                     [this](size_t a, size_t b) { return _ops[_members[a]].invoked_ms < _ops[_members[b]].invoked_ms; });
    for (const auto m : by_invocation) {
      const auto& operation = _ops[_members[m]];
      if (counts(operation) != Counts::unknown) continue;
      auto [at, fresh] = latest.try_emplace({operation.keys, operation.input}, m);
      if (!fresh) {
        _earlier[m] = at->second;
        at->second = m;
      }
    }
  }

  void remove(size_t at) {
    _next[_prev[at]] = _next[at];
    _prev[_next[at]] = _prev[at];
  }
  void restore(size_t at) {
    _next[_prev[at]] = at;
    _prev[_next[at]] = at;
  }
  // A call taken: it and its answer leave the list; undone in the reverse order.
  void lift(size_t call) {
    remove(call);
    if (_match[call] != end) remove(_match[call]);
  }
  void unlift(size_t call) {
    if (_match[call] != end) restore(_match[call]);
    restore(call);
  }

  const std::vector<Operation>& _ops;
  const Model& _model;
  std::vector<size_t> _members;
  std::vector<std::string> _keys;
  size_t _budget;
  std::vector<size_t> _next, _prev, _member_of, _match, _earlier;
  std::vector<bool> _call;
};

}  // namespace

Verdict check_linearizable(const History& history, const Model& model, size_t budget) {
  Verdict out;
  const auto& ops = history.operations();
  // Every key an operation touches, numbered; keys touched together are one group.
  std::map<std::string, size_t> key_at;
  for (const auto& operation : ops)
    if (counts(operation) != Counts::never)
      for (const auto& key : operation.keys) key_at.try_emplace(key, key_at.size());
  Groups groups{std::vector<size_t>(key_at.size())};
  std::iota(groups.parent.begin(), groups.parent.end(), size_t{0});
  for (const auto& operation : ops)
    if (counts(operation) != Counts::never)
      for (size_t i = 1; i < operation.keys.size(); ++i) groups.join(key_at.at(operation.keys[0]), key_at.at(operation.keys[i]));
  std::map<size_t, std::pair<std::vector<std::string>, std::vector<size_t>>> by_group;
  for (const auto& [key, at] : key_at) by_group[groups.find(at)].first.push_back(key);
  for (size_t i = 0; i < ops.size(); ++i)
    if (counts(ops[i]) != Counts::never && !ops[i].keys.empty()) by_group[groups.find(key_at.at(ops[i].keys[0]))].second.push_back(i);
  for (auto& [_, group] : by_group) {
    auto& [keys, members] = group;
    Search search(history, model, members, keys, budget > out.steps ? budget - out.steps : 0);
    if (!search.run(out)) {
      out.linearizable = false;
      out.keys = keys;
      return out;
    }
  }
  return out;
}

}  // namespace dannan::test::sim
