//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The harness's linearizability checker (TODO/2026-11-15-simulation-harness.md), on histories
// written by hand: what it must accept, and what it must refuse, naming the operation.

#include <gtest/gtest.h>

#include "dannan/test/fixtures/sim/history.h"
#include "dannan/test/fixtures/sim/linearizable.h"
#include "dannan/test/fixtures/sim/models.h"

namespace dannan::test::sim {
namespace {

Bytes text(const std::string& value) { return Bytes(value.begin(), value.end()); }

// One operation on `keys`, invoked at `from`, answered at `to` with `output`.
size_t done(History& history, uint64_t client, std::vector<std::string> keys, Bytes input, uint64_t from, uint64_t to, Bytes output) {
  const auto at = history.invoke(client, std::move(keys), std::move(input), from);
  history.answer(at, std::move(output), to);
  return at;
}

TEST(Linearizable, ASequentialHistoryIsExplained) {
  History history;
  done(history, 1, {"k"}, RegisterModel::write("a"), 10, 20, {});
  done(history, 2, {"k"}, RegisterModel::read(), 30, 40, text("a"));
  done(history, 1, {"k"}, RegisterModel::write("b"), 50, 60, {});
  done(history, 2, {"k"}, RegisterModel::read(), 70, 80, text("b"));
  const auto verdict = check_linearizable(history, RegisterModel{});
  EXPECT_TRUE(verdict.linearizable) << verdict.why;
}

// A read that returns a value overwritten before it began is refused, naming it.
TEST(Linearizable, AStaleReadIsRefused) {
  History history;
  done(history, 1, {"k"}, RegisterModel::write("a"), 10, 20, {});
  done(history, 1, {"k"}, RegisterModel::write("b"), 30, 40, {});
  const auto stale = done(history, 2, {"k"}, RegisterModel::read(), 50, 60, text("a"));
  const auto verdict = check_linearizable(history, RegisterModel{});
  EXPECT_FALSE(verdict.linearizable);
  EXPECT_FALSE(verdict.exhausted);
  EXPECT_EQ(verdict.keys, std::vector<std::string>{"k"});
  ASSERT_FALSE(verdict.unexplained.empty());
  EXPECT_EQ(verdict.unexplained[0], stale);
}

// Overlapping operations may take effect in either order.
TEST(Linearizable, ConcurrentOperationsMayTakeEffectInEitherOrder) {
  for (const auto& seen : {"a", "b"}) {
    History history;
    done(history, 1, {"k"}, RegisterModel::write("a"), 10, 50, {});
    done(history, 2, {"k"}, RegisterModel::write("b"), 20, 60, {});
    done(history, 3, {"k"}, RegisterModel::read(), 70, 80, text(seen));
    EXPECT_TRUE(check_linearizable(history, RegisterModel{}).linearizable) << seen;
  }
}

// An operation never answered may have taken effect, or not; one cancelled never did.
TEST(Linearizable, AnUnansweredOperationMayOrMayNotTakeEffectACancelledOneNever) {
  for (const auto& seen : {"", "a"}) {
    History history;
    history.invoke(1, {"k"}, RegisterModel::write("a"), 10);
    done(history, 2, {"k"}, RegisterModel::read(), 20, 30, text(seen));
    EXPECT_TRUE(check_linearizable(history, RegisterModel{}).linearizable) << "read '" << seen << "'";
  }
  History cancelled;
  const auto write = cancelled.invoke(1, {"k"}, RegisterModel::write("a"), 10);
  cancelled.cancel(write, 15);
  done(cancelled, 2, {"k"}, RegisterModel::read(), 20, 30, text("a"));
  EXPECT_FALSE(check_linearizable(cancelled, RegisterModel{}).linearizable);
}

// Kvasir's resolution: answered before it was decided, it counts as unanswered until
// resolved; resolved as not taken, it never took effect.
TEST(Linearizable, AProvisionalAnswerCountsOnlyOnceResolved) {
  History pending;
  const auto write = pending.invoke(1, {"k"}, RegisterModel::write("a"), 10);
  pending.answer(write, {}, 15, true);
  done(pending, 2, {"k"}, RegisterModel::read(), 20, 30, {});
  EXPECT_TRUE(check_linearizable(pending, RegisterModel{}).linearizable);
  pending.resolve(write, false, 40);
  done(pending, 2, {"k"}, RegisterModel::read(), 50, 60, text("a"));
  EXPECT_FALSE(check_linearizable(pending, RegisterModel{}).linearizable);

  // Resolved as taken, by its decision: a read before the decision may miss it.
  History taken;
  const auto decided = taken.invoke(1, {"k"}, RegisterModel::write("a"), 10);
  taken.answer(decided, {}, 15, true);
  taken.resolve(decided, true, 40);
  done(taken, 2, {"k"}, RegisterModel::read(), 20, 30, {});
  done(taken, 2, {"k"}, RegisterModel::read(), 50, 60, text("a"));
  EXPECT_TRUE(check_linearizable(taken, RegisterModel{}).linearizable);
}

// Mimir's: keys an operation touches together are one group; others are checked apart. A
// multi-key write is atomic: a read of both keys never sees half of it.
TEST(Linearizable, KeysTouchedTogetherAreOneGroup) {
  History history;
  done(history, 1, {"a", "b"}, RegisterModel::write("1"), 10, 20, {});
  done(history, 2, {"a", "b"}, RegisterModel::read(), 30, 40, RegisterModel::values({"1", "1"}));
  done(history, 3, {"c"}, RegisterModel::write("x"), 10, 20, {});
  done(history, 3, {"c"}, RegisterModel::read(), 30, 40, text("x"));
  EXPECT_TRUE(check_linearizable(history, RegisterModel{}).linearizable);

  History torn;
  done(torn, 1, {"a", "b"}, RegisterModel::write("1"), 10, 20, {});
  done(torn, 2, {"a", "b"}, RegisterModel::read(), 30, 40, RegisterModel::values({"1", ""}));
  const auto verdict = check_linearizable(torn, RegisterModel{});
  EXPECT_FALSE(verdict.linearizable);
  EXPECT_EQ(verdict.keys, (std::vector<std::string>{"a", "b"}));
}

// Mimir's allowed results: a model may allow more than one result and more than one next
// state (SPOP), and the search tries each.
class AnyOneModel final : public Model {
 public:
  // State: the set of values left, as sorted characters. 'p' pops any one, answered with it.
  std::string initial(const std::vector<std::string>&) const override { return "xyz"; }
  std::vector<std::string> step(const std::string& state, const Operation&, const Bytes* result) const override {
    std::vector<std::string> out;
    for (size_t i = 0; i < state.size(); ++i) {
      if (result && (result->size() != 1 || (*result)[0] != static_cast<uint8_t>(state[i]))) continue;
      out.push_back(state.substr(0, i) + state.substr(i + 1));
    }
    return out;
  }
};

TEST(Linearizable, AModelMayAllowSeveralResults) {
  History history;
  history.invoke(1, {"s"}, text("p"), 10);  // unanswered: any one, or none
  done(history, 2, {"s"}, text("p"), 20, 30, text("y"));
  done(history, 3, {"s"}, text("p"), 40, 50, text("z"));
  done(history, 4, {"s"}, text("p"), 60, 70, text("x"));
  EXPECT_TRUE(check_linearizable(history, AnyOneModel{}).linearizable);
  History twice;
  done(twice, 1, {"s"}, text("p"), 10, 20, text("y"));
  done(twice, 2, {"s"}, text("p"), 30, 40, text("y"));
  EXPECT_FALSE(check_linearizable(twice, AnyOneModel{}).linearizable);
}

// The counter agreement's simulations run: increments answered with the new value.
TEST(Linearizable, ACounterCountsEveryIncrementOnce) {
  History history;
  done(history, 1, {"c"}, Bytes{'i'}, 10, 40, CounterModel::number(2));
  done(history, 2, {"c"}, Bytes{'i'}, 20, 30, CounterModel::number(1));
  history.invoke(3, {"c"}, Bytes{'i'}, 50);
  done(history, 4, {"c"}, Bytes{'r'}, 60, 70, CounterModel::number(3));
  EXPECT_TRUE(check_linearizable(history, CounterModel{}).linearizable);
  History twice;
  done(twice, 1, {"c"}, Bytes{'i'}, 10, 20, CounterModel::number(1));
  done(twice, 2, {"c"}, Bytes{'i'}, 30, 40, CounterModel::number(1));
  EXPECT_FALSE(check_linearizable(twice, CounterModel{}).linearizable);
}

// A search that runs out says so rather than guessing.
TEST(Linearizable, ASearchThatRunsOutSaysSo) {
  History history;
  for (uint64_t client = 0; client < 12; ++client) history.invoke(client, {"k"}, RegisterModel::write(std::to_string(client)), 10);
  done(history, 99, {"k"}, RegisterModel::read(), 20, 30, text("none of them"));
  const auto verdict = check_linearizable(history, RegisterModel{}, 50);
  EXPECT_FALSE(verdict.linearizable);
  EXPECT_TRUE(verdict.exhausted);
}

}  // namespace
}  // namespace dannan::test::sim
