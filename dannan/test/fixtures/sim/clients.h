//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/serializable.h"
#include "dannan/test/fixtures/sim/world.h"

// Clients as actors (TODO/2026-11-15-simulation-harness.md, Kvasir's review): a client takes
// several steps, a transaction's statements with the world's other work between, so the world
// interleaves clients at statement granularity, every choice from its seed.
namespace dannan::test::sim {

class Actors {
 public:
  // One step of an actor: the milliseconds to its next, or none when it is done.
  using Step = std::function<std::optional<uint64_t>()>;

  explicit Actors(World& world) : _world(world) {}
  Actors(const Actors&) = delete;
  Actors& operator=(const Actors&) = delete;
  // Steps booked in the world after this is gone do nothing.
  ~Actors() { _state->alive = false; }

  // Runs `step` after `delay`, then after each delay it returns, until it returns none.
  void spawn(Step step, uint64_t delay = 0) {
    ++_state->running;
    auto actor = std::make_shared<Step>(std::move(step));
    book(std::move(actor), delay);
  }
  // Actors not yet done.
  size_t running() const noexcept { return _state->running; }

 private:
  struct State {
    bool alive = true;
    size_t running = 0;
  };
  void book(std::shared_ptr<Step> actor, uint64_t delay) {
    _world.after(delay, [this, state = _state, actor = std::move(actor)]() mutable {
      if (!state->alive) return;
      if (const auto next = (*actor)()) return book(std::move(actor), *next);
      --state->running;
    });
  }

  World& _world;
  std::shared_ptr<State> _state = std::make_shared<State>();
};

// A random transaction over `keys`: one to `most` statements, reads and appends evenly, each
// append's element the next of `elements` (unique across the history).
inline std::vector<Statement> random_transaction(Random& random, const std::vector<std::string>& keys, uint64_t& elements, size_t most = 4) {
  std::vector<Statement> out(1 + random.below(most));
  for (auto& statement : out) {
    statement.key = keys[random.below(keys.size())];
    if (random.one_in(2)) {
      statement.kind = Statement::Kind::append;
      statement.element = ++elements;
    }
  }
  return out;
}

}  // namespace dannan::test::sim
