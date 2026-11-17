//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The transaction checker (TODO/2026-11-15-simulation-harness.md, Kvasir's review), from
// Adya's definitions: each history below holds exactly the anomaly it is named for, and each
// level refuses what it forbids and nothing more.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/clients.h"
#include "dannan/test/fixtures/sim/history.h"
#include "dannan/test/fixtures/sim/serializable.h"

namespace dannan::test::sim {
namespace {

Statement read(std::string key, std::vector<uint64_t> list) { return Statement{Statement::Kind::read, std::move(key), 0, std::move(list)}; }
Statement append(std::string key, uint64_t element) { return Statement{Statement::Kind::append, std::move(key), element, {}}; }

enum class Ends { committed, aborted, unknown };

// One transaction into the history, invoked at `from` and, unless unknown, ended at `to`.
size_t txn(History& history, uint64_t client, const std::vector<Statement>& statements, uint64_t from = 0, uint64_t to = 1, Ends ends = Ends::committed) {
  std::vector<std::string> keys;
  for (const auto& statement : statements) keys.push_back(statement.key);
  const auto index = history.invoke(client, keys, encode_statements(statements), from);
  if (ends == Ends::committed) history.answer(index, encode_reads(statements), to);
  if (ends == Ends::aborted) history.cancel(index, to);
  return index;
}

bool holds(const History& history, Isolation level, Order order = Order::none) { return check_isolation(history, level, order).ok; }

std::string first(const History& history, Isolation level, Order order = Order::none) {
  const auto verdict = check_isolation(history, level, order);
  return verdict.anomalies.empty() ? std::string("none") : verdict.anomalies.front().kind;
}

TEST(Isolation, StatementsAndReadsRoundTrip) {
  History history;
  const std::vector<Statement> statements{append("x", 7), read("x", {3, 7}), read("y", {})};
  txn(history, 1, statements);
  const auto back = decode(history.operations()[0]);
  ASSERT_EQ(back.size(), 3u);
  EXPECT_EQ(back[0].kind, Statement::Kind::append);
  EXPECT_EQ(back[0].element, 7u);
  EXPECT_EQ(back[1].list, (std::vector<uint64_t>{3, 7}));
  EXPECT_EQ(back[2].key, "y");
  EXPECT_TRUE(back[2].list.empty());
}

TEST(Isolation, ASerialHistoryKeepsEveryLevel) {
  History history;
  txn(history, 1, {append("x", 1), append("y", 2)}, 0, 1);
  txn(history, 2, {read("x", {1}), append("x", 3)}, 2, 3);
  txn(history, 3, {read("x", {1, 3}), read("y", {2})}, 4, 5);
  for (const auto order : {Order::none, Order::session, Order::real_time}) {
    EXPECT_TRUE(holds(history, Isolation::serializable, order));
    EXPECT_TRUE(holds(history, Isolation::snapshot, order));
    EXPECT_TRUE(holds(history, Isolation::read_committed, order));
  }
}

// G0, write cycles: two writers' appends ordered one way on x and the other on y.
TEST(Isolation, AWriteCycleIsG0AndNoLevelAllowsIt) {
  History history;
  txn(history, 1, {append("x", 1), append("y", 3)});
  txn(history, 2, {append("x", 2), append("y", 4)});
  txn(history, 3, {read("x", {1, 2}), read("y", {4, 3})});
  const auto verdict = check_isolation(history, Isolation::read_committed);
  ASSERT_FALSE(verdict.ok);
  EXPECT_EQ(verdict.anomalies.front().kind, "G0");
  EXPECT_EQ(verdict.anomalies.front().edges, (std::vector<std::string>{"ww", "ww"}));
  EXPECT_EQ(verdict.anomalies.front().transactions.size(), 2u);
  EXPECT_FALSE(holds(history, Isolation::snapshot));
  EXPECT_FALSE(holds(history, Isolation::serializable));
}

// G1a, an aborted read: a committed transaction saw an aborted one's append.
TEST(Isolation, ReadingAnAbortedWriteIsG1a) {
  History history;
  txn(history, 1, {append("x", 1)}, 0, 1, Ends::aborted);
  txn(history, 2, {read("x", {1})});
  EXPECT_EQ(first(history, Isolation::read_committed), "G1a");
}

// G1b, an intermediate read: a transaction saw another's state between two of its appends.
TEST(Isolation, ReadingAnIntermediateWriteIsG1b) {
  History history;
  txn(history, 1, {append("x", 1), append("x", 2)});
  txn(history, 2, {read("x", {1})});
  EXPECT_EQ(first(history, Isolation::read_committed), "G1b");
}

// G1c, a cycle of write-read and write-write edges: each saw the other's write.
TEST(Isolation, EachSeeingTheOthersWriteIsG1c) {
  History history;
  txn(history, 1, {append("x", 1), read("y", {2})});
  txn(history, 2, {append("y", 2), read("x", {1})});
  EXPECT_EQ(first(history, Isolation::read_committed), "G1c");
}

// G-single, read skew: one anti-dependency in the cycle. T1 read x before T2's append and y
// after it. Read committed allows it; snapshot does not.
TEST(Isolation, ReadSkewIsGSingleAllowedOnlyAtReadCommitted) {
  History history;
  txn(history, 1, {read("x", {}), read("y", {2})});
  txn(history, 2, {append("x", 1), append("y", 2)});
  txn(history, 3, {read("x", {1})});
  EXPECT_TRUE(holds(history, Isolation::read_committed));
  EXPECT_EQ(first(history, Isolation::snapshot), "G-single");
  EXPECT_FALSE(holds(history, Isolation::serializable));
}

// G2, write skew: two anti-dependencies. Snapshot allows it (Kvasir's review: a serial-order
// search would refuse a correct snapshot history); serializable does not.
TEST(Isolation, WriteSkewIsG2AllowedAtSnapshot) {
  History history;
  txn(history, 1, {read("x", {}), read("y", {}), append("x", 1)});
  txn(history, 2, {read("x", {}), read("y", {}), append("y", 2)});
  txn(history, 3, {read("x", {1}), read("y", {2})});
  EXPECT_TRUE(holds(history, Isolation::read_committed));
  EXPECT_TRUE(holds(history, Isolation::snapshot));
  EXPECT_EQ(first(history, Isolation::serializable), "G2");
}

// A stale read: serializable alone (T2 before T1), not once real time orders them.
TEST(Isolation, AStaleReadIsRefusedOnlyWithRealTime) {
  History history;
  txn(history, 1, {append("x", 1)}, 0, 10);
  txn(history, 2, {read("x", {})}, 20, 30);
  txn(history, 3, {read("x", {1})}, 40, 50);
  EXPECT_TRUE(holds(history, Isolation::serializable));
  EXPECT_TRUE(holds(history, Isolation::serializable, Order::session));
  const auto verdict = check_isolation(history, Isolation::serializable, Order::real_time);
  ASSERT_FALSE(verdict.ok);
  EXPECT_EQ(verdict.anomalies.front().kind, "G-single");
  EXPECT_NE(std::find(verdict.anomalies.front().edges.begin(), verdict.anomalies.front().edges.end(), "real-time"), verdict.anomalies.front().edges.end());
}

// A client that does not see its own commit breaks session order, and nothing else.
TEST(Isolation, NotSeeingOnesOwnCommitIsRefusedWithSessionOrder) {
  History history;
  txn(history, 1, {append("x", 1)}, 0, 10);
  txn(history, 1, {read("x", {})}, 20, 30);
  txn(history, 2, {read("x", {1})}, 40, 50);
  EXPECT_TRUE(holds(history, Isolation::serializable));
  EXPECT_FALSE(holds(history, Isolation::serializable, Order::session));
}

// Never answered: committed if anything it appended was read, else as if it never ran. Its
// reads, never returned, say nothing.
TEST(Isolation, AnUnansweredTransactionCountsByWhetherItsWriteWasSeen) {
  History seen;
  txn(seen, 1, {append("x", 1)}, 0, 0, Ends::unknown);
  txn(seen, 2, {read("x", {1})}, 5, 6);
  EXPECT_TRUE(holds(seen, Isolation::serializable, Order::real_time));
  History unseen;
  txn(unseen, 1, {append("x", 1), read("y", {})}, 0, 0, Ends::unknown);
  txn(unseen, 2, {read("x", {})}, 5, 6);
  EXPECT_TRUE(holds(unseen, Isolation::serializable, Order::real_time));
}

// Two reads that are not one a prefix of the other: no version order explains them.
TEST(Isolation, ReadsThatDisagreeOnAKeysOrderAreRefused) {
  History history;
  txn(history, 1, {append("x", 1)});
  txn(history, 2, {append("x", 2)});
  txn(history, 3, {read("x", {1, 2})});
  txn(history, 4, {read("x", {2, 1})});
  EXPECT_EQ(first(history, Isolation::read_committed), "incompatible-order");
}

// A transaction's read after its own append must end with that append.
TEST(Isolation, ATransactionThatMissesItsOwnAppendIsRefused) {
  History history;
  txn(history, 1, {append("x", 1), read("x", {})});
  EXPECT_EQ(first(history, Isolation::read_committed), "internal");
}

// An element is unique to one append; two appends of it are a defect of the client.
TEST(Isolation, AnElementAppendedTwiceIsRefused) {
  History history;
  txn(history, 1, {append("x", 1)});
  txn(history, 2, {append("x", 1)});
  EXPECT_EQ(first(history, Isolation::read_committed), "duplicate-element");
}

// --- Stores of a known level, run by client actors ----------------------------------------

// A store in memory whose level is known: serial (one transaction at a time), snapshot (reads
// from the snapshot at begin, first committer wins), read committed (reads the latest commits),
// and dirty (reads appends not yet committed, which an abort takes back).
class KnownStore {
 public:
  enum class Mode { serial, snapshot, read_committed, dirty };
  explicit KnownStore(Mode mode) : _mode(mode) {}

  struct Txn {
    std::map<std::string, std::vector<uint64_t>> snapshot;
    std::map<std::string, std::vector<uint64_t>> mine;
    uint64_t began = 0;
  };

  // False while a serial store runs another.
  bool begin(Txn& txn) {
    if (_mode == Mode::serial) {
      if (_locked) return false;
      _locked = true;
    }
    txn.began = _commits;
    if (_mode == Mode::snapshot) txn.snapshot = _committed;
    return true;
  }
  std::vector<uint64_t> read(Txn& txn, const std::string& key) {
    if (_mode == Mode::dirty) return _live[key];
    auto out = _mode == Mode::snapshot ? txn.snapshot[key] : _committed[key];
    // Looked up, not inserted: a read key in `mine` would put it under first-committer-wins,
    // which would make the snapshot store serializable.
    if (const auto it = txn.mine.find(key); it != txn.mine.end()) out.insert(out.end(), it->second.begin(), it->second.end());
    return out;
  }
  void append(Txn& txn, const std::string& key, uint64_t element) {
    txn.mine[key].push_back(element);
    if (_mode == Mode::dirty) _live[key].push_back(element);
  }
  // False if it aborted: a snapshot store's first committer won.
  bool commit(Txn& txn) {
    if (_mode == Mode::serial) _locked = false;
    if (_mode == Mode::snapshot)
      for (const auto& [key, elements] : txn.mine)
        if (_last[key] > txn.began) return false;
    for (const auto& [key, elements] : txn.mine) {
      auto& list = _committed[key];
      list.insert(list.end(), elements.begin(), elements.end());
      _last[key] = ++_commits;
    }
    return true;
  }
  void abort(Txn& txn) {
    if (_mode == Mode::serial) _locked = false;
    if (_mode != Mode::dirty) return;
    for (const auto& [key, elements] : txn.mine) {
      auto& live = _live[key];
      for (const auto element : elements) live.erase(std::find(live.begin(), live.end(), element));
    }
  }

 private:
  const Mode _mode;
  std::map<std::string, std::vector<uint64_t>> _committed;
  std::map<std::string, std::vector<uint64_t>> _live;
  std::map<std::string, uint64_t> _last;
  uint64_t _commits = 0;
  bool _locked = false;
};

// Clients running random transactions against a store, a statement a step, each one aborting
// one time in ten of its own accord.
History run_clients(KnownStore::Mode mode, uint64_t seed, size_t clients = 4, size_t each = 25) {
  World world(seed);
  KnownStore store(mode);
  History history;
  Actors actors(world);
  const std::vector<std::string> keys{"x", "y", "z"};
  uint64_t elements = 0;
  for (size_t c = 0; c < clients; ++c) {
    struct Client {
      size_t left;
      std::vector<Statement> statements;
      KnownStore::Txn txn;
      size_t index = 0;
      size_t next = 0;
      bool begun = false;
    };
    auto client = std::make_shared<Client>(Client{each, {}, {}});
    actors.spawn([&, client, c]() -> std::optional<uint64_t> {
      auto& me = *client;
      const auto pause = [&] { return 1 + world.random.below(3); };
      if (me.statements.empty()) {
        if (me.left == 0) return std::nullopt;
        --me.left;
        me.statements = random_transaction(world.random, keys, elements);
        me.index = history.invoke(c, {}, encode_statements(me.statements), world.now_ms());
        me.txn = {};
        me.next = 0;
        me.begun = false;
        return pause();
      }
      if (!me.begun) {
        if (!store.begin(me.txn)) return 1;
        me.begun = true;
        return pause();
      }
      if (me.next < me.statements.size()) {
        auto& statement = me.statements[me.next++];
        if (statement.kind == Statement::Kind::read)
          statement.list = store.read(me.txn, statement.key);
        else
          store.append(me.txn, statement.key, statement.element);
        return pause();
      }
      if (world.random.one_in(10)) {
        store.abort(me.txn);
        history.cancel(me.index, world.now_ms());
      } else if (store.commit(me.txn)) {
        history.answer(me.index, encode_reads(me.statements), world.now_ms());
      } else {
        history.cancel(me.index, world.now_ms());
      }
      me.statements.clear();
      return pause();
    });
  }
  world.run_until([&] { return actors.running() == 0; }, 10'000'000);
  EXPECT_EQ(actors.running(), 0u);
  return history;
}

// Seeds of `mode` whose history fails `level` under `order`, of 40.
size_t failing(KnownStore::Mode mode, Isolation level, Order order) {
  size_t out = 0;
  for (uint64_t seed = 1; seed <= 40; ++seed) {
    const auto verdict = check_isolation(run_clients(mode, seed), level, order);
    if (!verdict.ok) ++out;
  }
  return out;
}

// Each store keeps its own level on every seed, and the checker finds what the level below
// allows on some: write skew under snapshot, read skew under read committed, an aborted or
// intermediate read when a store reads what is not committed.
TEST(Isolation, AStoreOfAKnownLevelPassesItAndTheLevelAboveIsBroken) {
  EXPECT_EQ(failing(KnownStore::Mode::serial, Isolation::serializable, Order::real_time), 0u);
  EXPECT_EQ(failing(KnownStore::Mode::snapshot, Isolation::snapshot, Order::session), 0u);
  EXPECT_GT(failing(KnownStore::Mode::snapshot, Isolation::serializable, Order::session), 0u);
  EXPECT_EQ(failing(KnownStore::Mode::read_committed, Isolation::read_committed, Order::session), 0u);
  EXPECT_GT(failing(KnownStore::Mode::read_committed, Isolation::snapshot, Order::session), 0u);
  EXPECT_GT(failing(KnownStore::Mode::dirty, Isolation::read_committed, Order::none), 0u);
}

// A seed replays exactly: the same history, operation for operation.
TEST(Isolation, AClientsRunReplaysFromItsSeed) {
  const auto a = run_clients(KnownStore::Mode::snapshot, 7);
  const auto b = run_clients(KnownStore::Mode::snapshot, 7);
  ASSERT_EQ(a.operations().size(), b.operations().size());
  for (size_t i = 0; i < a.operations().size(); ++i) {
    EXPECT_EQ(a.operations()[i].input, b.operations()[i].input);
    EXPECT_EQ(a.operations()[i].output, b.operations()[i].output);
    EXPECT_EQ(a.operations()[i].answered_ms, b.operations()[i].answered_ms);
  }
}

}  // namespace
}  // namespace dannan::test::sim
