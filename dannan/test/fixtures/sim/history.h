//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The history a simulation records (TODO/2026-11-15-simulation-harness.md, "History and
// checkers"): every client operation's invocation and what became of it, and every fault the
// world injected, each at the world's time. The checkers read it; nothing here judges it.
namespace dannan::test::sim {

using Bytes = std::vector<uint8_t>;

// What a client was told of an operation.
enum class Answer : uint8_t {
  // Never answered (a timeout, a client gone): it may or may not have taken effect.
  none,
  // Answered with a result.
  ok,
  // Known to have taken no effect (Mimir's review: a client that closed while parked, a
  // BLPOP timed out with nil).
  cancelled,
};

struct Operation {
  // The client's own id for it: a retry inside the client is one operation.
  uint64_t id = 0;
  uint64_t client = 0;
  // The keys it touches; an operation over several is atomic across them.
  std::vector<std::string> keys;
  // The owner's encoding of what it asked, and of its result once answered.
  Bytes input;
  Bytes output;
  uint64_t invoked_ms = 0;
  std::optional<uint64_t> answered_ms;
  Answer answer = Answer::none;
  // Answered before the cluster decided it (Kvasir's review: `ephemeral` and `persisted`
  // commits); until resolved it counts as never answered.
  bool provisional = false;
  // The decision that came later: taken, or not (then it took no effect).
  std::optional<bool> resolved;
  std::optional<uint64_t> resolved_ms;
  // The node it ran on, for the faults.
  uint64_t node = 0;
};

// A fault the world injected, as an event in the history (Kvasir's review): durability is
// judged against these.
struct Fault {
  uint64_t at_ms = 0;
  // "kill", "restart", "wipe", "lying_sync", "torn", "partition", "heal", ...
  std::string kind;
  std::vector<uint64_t> nodes;
  std::string detail;
};

class History {
 public:
  // A new operation, invoked now; its index, by which it is answered.
  size_t invoke(uint64_t client, std::vector<std::string> keys, Bytes input, uint64_t now_ms, uint64_t node = 0, std::optional<uint64_t> id = {}) {
    Operation operation;
    operation.id = id.value_or(_operations.size());
    operation.client = client;
    operation.keys = std::move(keys);
    operation.input = std::move(input);
    operation.invoked_ms = now_ms;
    operation.node = node;
    _operations.push_back(std::move(operation));
    return _operations.size() - 1;
  }
  // Answered with `output`; `provisional` if the cluster had not decided it yet.
  void answer(size_t index, Bytes output, uint64_t now_ms, bool provisional = false) {
    auto& operation = at(index);
    operation.output = std::move(output);
    operation.answered_ms = now_ms;
    operation.answer = Answer::ok;
    operation.provisional = provisional;
  }
  // Known to have taken no effect.
  void cancel(size_t index, uint64_t now_ms) {
    auto& operation = at(index);
    operation.answered_ms = now_ms;
    operation.answer = Answer::cancelled;
  }
  // A provisional answer decided: taken, or not.
  void resolve(size_t index, bool taken, uint64_t now_ms) {
    auto& operation = at(index);
    operation.resolved = taken;
    operation.resolved_ms = now_ms;
  }
  void fault(Fault fault) { _faults.push_back(std::move(fault)); }

  const std::vector<Operation>& operations() const noexcept { return _operations; }
  const std::vector<Fault>& faults() const noexcept { return _faults; }

 private:
  Operation& at(size_t index) {
    if (index >= _operations.size()) throw std::out_of_range("no such operation in the history");
    return _operations[index];
  }
  std::vector<Operation> _operations;
  std::vector<Fault> _faults;
};

}  // namespace dannan::test::sim
