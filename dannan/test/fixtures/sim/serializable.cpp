//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/test/fixtures/sim/serializable.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace dannan::test::sim {
namespace {

void put_u32(Bytes& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void put_u64(Bytes& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

struct Reader {
  const Bytes& bytes;
  size_t at = 0;
  uint64_t take(size_t width) {
    if (bytes.size() - at < width) throw std::invalid_argument("a transaction's encoding ends early");
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= static_cast<uint64_t>(bytes[at + i]) << (8 * i);
    at += width;
    return value;
  }
  bool done() const { return at == bytes.size(); }
};

// What a transaction counts as.
enum class Status { committed, aborted, unknown };

Status status_of(const Operation& operation) {
  if (operation.answer == Answer::cancelled) return Status::aborted;
  if (operation.resolved && !*operation.resolved) return Status::aborted;
  if (operation.answer == Answer::ok && (!operation.provisional || operation.resolved)) return Status::committed;
  return Status::unknown;
}

enum class Edge : uint8_t { ww, wr, rw, order };

struct Graph {
  // Transactions are nodes 0..transactions-1; the rest are real-time barriers.
  size_t transactions = 0;
  std::vector<std::vector<std::pair<size_t, Edge>>> out;
  size_t add() {
    out.emplace_back();
    return out.size() - 1;
  }
  void link(size_t from, size_t to, Edge kind) {
    if (from == to) return;
    out[from].emplace_back(to, kind);
  }
};

// A path from `from` to `to` over edges `allowed` admits, as (node, edge into it) after `from`;
// empty if none.
template <typename Allowed>
std::vector<std::pair<size_t, Edge>> path(const Graph& graph, size_t from, size_t to, const std::vector<bool>& within, Allowed allowed) {
  std::vector<size_t> came(graph.out.size(), SIZE_MAX);
  std::vector<Edge> by(graph.out.size(), Edge::ww);
  std::deque<size_t> queue{from};
  came[from] = from;
  while (!queue.empty()) {
    const auto at = queue.front();
    queue.pop_front();
    if (at == to && at != from) break;
    for (const auto& [next, kind] : graph.out[at]) {
      if (!within[next] || !allowed(kind) || came[next] != SIZE_MAX) continue;
      came[next] = at;
      by[next] = kind;
      queue.push_back(next);
    }
  }
  if (from == to || came[to] == SIZE_MAX) return {};
  std::vector<std::pair<size_t, Edge>> out;
  for (size_t at = to; at != from; at = came[at]) out.emplace_back(at, by[at]);
  std::reverse(out.begin(), out.end());
  return out;
}

// Tarjan's strongly connected components; each node's component.
std::vector<size_t> components(const Graph& graph) {
  const size_t n = graph.out.size();
  std::vector<size_t> index(n, SIZE_MAX), low(n, 0), component(n, SIZE_MAX);
  std::vector<bool> on(n, false);
  std::vector<size_t> stack;
  size_t next = 0, count = 0;
  for (size_t root = 0; root < n; ++root) {
    if (index[root] != SIZE_MAX) continue;
    std::vector<std::pair<size_t, size_t>> work{{root, 0}};
    while (!work.empty()) {
      auto& [at, edge] = work.back();
      if (edge == 0) {
        index[at] = low[at] = next++;
        stack.push_back(at);
        on[at] = true;
      }
      if (edge < graph.out[at].size()) {
        const auto to = graph.out[at][edge++].first;
        if (index[to] == SIZE_MAX) {
          work.emplace_back(to, 0);
        } else if (on[to]) {
          low[at] = std::min(low[at], index[to]);
        }
        continue;
      }
      if (low[at] == index[at]) {
        for (;;) {
          const auto member = stack.back();
          stack.pop_back();
          on[member] = false;
          component[member] = count;
          if (member == at) break;
        }
        ++count;
      }
      const auto done = at;
      work.pop_back();
      if (!work.empty()) low[work.back().first] = std::min(low[work.back().first], low[done]);
    }
  }
  return component;
}

const char* name_of(Edge edge, Order order) {
  switch (edge) {
    case Edge::ww:
      return "ww";
    case Edge::wr:
      return "wr";
    case Edge::rw:
      return "rw";
    case Edge::order:
      return order == Order::session ? "session" : "real-time";
  }
  return "?";
}

}  // namespace

Bytes encode_statements(const std::vector<Statement>& statements) {
  Bytes out;
  put_u32(out, static_cast<uint32_t>(statements.size()));
  for (const auto& statement : statements) {
    out.push_back(static_cast<uint8_t>(statement.kind));
    put_u32(out, static_cast<uint32_t>(statement.key.size()));
    out.insert(out.end(), statement.key.begin(), statement.key.end());
    put_u64(out, statement.element);
  }
  return out;
}

Bytes encode_reads(const std::vector<Statement>& statements) {
  Bytes out;
  for (const auto& statement : statements) {
    if (statement.kind != Statement::Kind::read) continue;
    put_u32(out, static_cast<uint32_t>(statement.list.size()));
    for (const auto element : statement.list) put_u64(out, element);
  }
  return out;
}

std::vector<Statement> decode(const Operation& operation) {
  Reader in{operation.input};
  std::vector<Statement> out(in.take(4));
  for (auto& statement : out) {
    const auto kind = in.take(1);
    if (kind > 1) throw std::invalid_argument("a statement of no known kind");
    statement.kind = static_cast<Statement::Kind>(kind);
    const auto length = in.take(4);
    if (operation.input.size() - in.at < length) throw std::invalid_argument("a transaction's encoding ends early");
    statement.key.assign(operation.input.begin() + static_cast<std::ptrdiff_t>(in.at), operation.input.begin() + static_cast<std::ptrdiff_t>(in.at + length));
    in.at += length;
    statement.element = in.take(8);
  }
  if (!in.done()) throw std::invalid_argument("a transaction's encoding runs on");
  if (operation.answer != Answer::ok) return out;
  Reader reads{operation.output};
  for (auto& statement : out) {
    if (statement.kind != Statement::Kind::read) continue;
    statement.list.resize(reads.take(4));
    for (auto& element : statement.list) element = reads.take(8);
  }
  if (!reads.done()) throw std::invalid_argument("a transaction's reads run on");
  return out;
}

IsolationVerdict check_isolation(const History& history, Isolation level, Order order) {
  IsolationVerdict verdict;
  const auto report = [&](std::string kind, std::vector<size_t> transactions, std::string why, std::vector<std::string> edges = {}) {
    verdict.ok = false;
    verdict.anomalies.push_back(Anomaly{std::move(kind), std::move(transactions), std::move(edges), std::move(why)});
  };
  const auto& operations = history.operations();
  std::vector<std::vector<Statement>> statements;
  std::vector<Status> status;
  for (const auto& operation : operations) {
    statements.push_back(decode(operation));
    status.push_back(status_of(operation));
  }

  // Every element's writer, and each writer's appends per key in order.
  struct Written {
    size_t writer;
    // Its place among the writer's appends to the key, and how many it made.
    size_t nth;
    size_t of;
  };
  std::map<uint64_t, Written> writer;
  for (size_t t = 0; t < operations.size(); ++t) {
    std::map<std::string, std::vector<uint64_t>> mine;
    for (const auto& statement : statements[t])
      if (statement.kind == Statement::Kind::append) mine[statement.key].push_back(statement.element);
    for (const auto& [key, elements] : mine)
      for (size_t i = 0; i < elements.size(); ++i) {
        const auto [at, fresh] = writer.emplace(elements[i], Written{t, i, elements.size()});
        if (!fresh) report("duplicate-element", {at->second.writer, t}, "element " + std::to_string(elements[i]) + " is appended twice");
      }
  }

  // The reads that count: a committed transaction's. Each is split into what it saw of others
  // (external) after its own appends so far are taken off the end, which must match them.
  struct Seen {
    size_t reader;
    std::string key;
    std::vector<uint64_t> list;
  };
  std::vector<Seen> seen;
  for (size_t t = 0; t < operations.size(); ++t) {
    if (status[t] != Status::committed) continue;
    std::map<std::string, std::vector<uint64_t>> own;
    for (const auto& statement : statements[t]) {
      if (statement.kind == Statement::Kind::append) {
        own[statement.key].push_back(statement.element);
        continue;
      }
      const auto& mine = own[statement.key];
      if (statement.list.size() < mine.size() || !std::equal(mine.begin(), mine.end(), statement.list.end() - static_cast<std::ptrdiff_t>(mine.size()))) {
        report("internal", {t}, "a read of " + statement.key + " misses the transaction's own appends");
        continue;
      }
      seen.push_back(Seen{t, statement.key, std::vector<uint64_t>(statement.list.begin(), statement.list.end() - static_cast<std::ptrdiff_t>(mine.size()))});
    }
  }

  // An unknown transaction committed if anything it appended was read.
  for (const auto& read : seen)
    for (const auto element : read.list)
      if (const auto it = writer.find(element); it != writer.end() && status[it->second.writer] == Status::unknown)
        status[it->second.writer] = Status::committed;

  // Each key's version order: its longest read; every other read a prefix of it.
  std::map<std::string, std::vector<uint64_t>> versions;
  for (const auto& read : seen)
    if (read.list.size() > versions[read.key].size()) versions[read.key] = read.list;
  std::vector<bool> consistent(seen.size(), true);
  for (size_t i = 0; i < seen.size(); ++i) {
    const auto& longest = versions[seen[i].key];
    if (!std::equal(seen[i].list.begin(), seen[i].list.end(), longest.begin())) {
      consistent[i] = false;
      report("incompatible-order", {seen[i].reader}, "a read of " + seen[i].key + " is not a prefix of the longest");
    }
  }

  // The aborted and intermediate reads, and elements no one appended.
  for (size_t i = 0; i < seen.size(); ++i) {
    const auto& read = seen[i];
    for (const auto element : read.list) {
      const auto it = writer.find(element);
      if (it == writer.end()) {
        report("garbage", {read.reader}, "element " + std::to_string(element) + " of " + read.key + " was never appended");
      } else if (status[it->second.writer] == Status::aborted) {
        report("G1a", {it->second.writer, read.reader}, "element " + std::to_string(element) + " of " + read.key + " is an aborted append");
      }
    }
    if (read.list.empty()) continue;
    const auto it = writer.find(read.list.back());
    if (it != writer.end() && it->second.writer != read.reader && it->second.nth + 1 < it->second.of)
      report("G1b", {it->second.writer, read.reader}, "a read of " + read.key + " ends inside another transaction's appends");
  }

  // The graph over committed transactions.
  Graph graph;
  graph.transactions = operations.size();
  graph.out.resize(operations.size());
  const auto committed = [&](size_t t) { return status[t] == Status::committed; };
  const auto writer_of = [&](uint64_t element) -> std::optional<size_t> {
    const auto it = writer.find(element);
    if (it == writer.end() || !committed(it->second.writer)) return std::nullopt;
    return it->second.writer;
  };
  for (const auto& [key, list] : versions)
    for (size_t i = 0; i + 1 < list.size(); ++i) {
      const auto a = writer_of(list[i]), b = writer_of(list[i + 1]);
      if (a && b) graph.link(*a, *b, Edge::ww);
    }
  for (size_t i = 0; i < seen.size(); ++i) {
    if (!consistent[i]) continue;
    const auto& read = seen[i];
    if (!read.list.empty())
      if (const auto from = writer_of(read.list.back())) graph.link(*from, read.reader, Edge::wr);
    const auto& list = versions[read.key];
    if (read.list.size() < list.size())
      if (const auto to = writer_of(list[read.list.size()])) graph.link(read.reader, *to, Edge::rw);
  }
  if (order == Order::session) {
    std::map<uint64_t, std::vector<size_t>> sessions;
    for (size_t t = 0; t < operations.size(); ++t)
      if (committed(t)) sessions[operations[t].client].push_back(t);
    for (auto& [client, mine] : sessions) {
      std::stable_sort(mine.begin(), mine.end(), [&](size_t a, size_t b) { return operations[a].invoked_ms < operations[b].invoked_ms; });
      for (size_t i = 0; i + 1 < mine.size(); ++i) graph.link(mine[i], mine[i + 1], Edge::order);
    }
  } else if (order == Order::real_time) {
    // A barrier per answer time, chained: a transaction reaches its answer's barrier, and each
    // barrier reaches the transactions invoked after it, so T1 reaches T2 by them exactly when
    // T1 was answered before T2 was invoked.
    std::map<uint64_t, size_t> barrier;
    for (size_t t = 0; t < operations.size(); ++t)
      if (committed(t) && operations[t].answered_ms) barrier.emplace(*operations[t].answered_ms, 0);
    size_t previous = SIZE_MAX;
    for (auto& [at, node] : barrier) {
      node = graph.add();
      if (previous != SIZE_MAX) graph.link(previous, node, Edge::order);
      previous = node;
    }
    for (size_t t = 0; t < operations.size(); ++t) {
      if (!committed(t)) continue;
      if (operations[t].answered_ms) graph.link(t, barrier.at(*operations[t].answered_ms), Edge::order);
      // The latest answer strictly before this invocation.
      auto it = barrier.lower_bound(operations[t].invoked_ms);
      if (it != barrier.begin()) graph.link(std::prev(it)->second, t, Edge::order);
    }
  }

  // Cycles, only within strongly connected components of the whole graph.
  const auto component = components(graph);
  const std::vector<bool> everywhere(graph.out.size(), true);
  // A closed walk, as (node, edge into it), its last node the first's predecessor: reported from
  // a transaction, with the barriers between transactions read as one order edge.
  const auto render = [&](const char* kind, std::vector<std::pair<size_t, Edge>> walk, const char* why) {
    for (size_t i = 0; i < walk.size(); ++i)
      if (walk[i].first < graph.transactions) {
        std::rotate(walk.begin(), walk.begin() + static_cast<std::ptrdiff_t>(i + 1), walk.end());
        break;
      }
    const auto start = walk.back().first;
    std::vector<size_t> transactions{start};
    std::vector<std::string> edges;
    bool through_order = false;
    for (size_t i = 0; i < walk.size(); ++i) {
      const auto [node, edge] = walk[i];
      if (node >= graph.transactions) {
        through_order = true;
        continue;
      }
      edges.push_back(name_of(through_order ? Edge::order : edge, order));
      through_order = false;
      if (i + 1 < walk.size()) transactions.push_back(node);
    }
    report(kind, std::move(transactions), why, std::move(edges));
  };
  // A cycle through an edge u->v that `first` admits, back from v to u over edges `back` admits:
  // reported once per component, and the components it was found in.
  const auto find = [&](const char* kind, auto first, auto back, const char* why, const std::set<size_t>& skip = {}) {
    std::set<size_t> found;
    for (size_t u = 0; u < graph.out.size(); ++u)
      for (const auto& [v, edge] : graph.out[u]) {
        if (!first(edge) || component[u] != component[v] || found.count(component[u]) || skip.count(component[u])) continue;
        const auto rest = path(graph, v, u, everywhere, back);
        if (rest.empty()) continue;
        found.insert(component[u]);
        std::vector<std::pair<size_t, Edge>> walk{{v, edge}};
        walk.insert(walk.end(), rest.begin(), rest.end());
        render(kind, std::move(walk), why);
      }
    return found;
  };
  const auto is = [](std::initializer_list<Edge> kinds) {
    return [kinds = std::vector<Edge>(kinds)](Edge edge) { return std::find(kinds.begin(), kinds.end(), edge) != kinds.end(); };
  };
  find("G0", is({Edge::ww}), is({Edge::ww, Edge::order}), "a cycle of writes");
  find("G1c", is({Edge::wr}), is({Edge::ww, Edge::wr, Edge::order}), "a cycle of writes and reads");
  if (level == Isolation::read_committed) return verdict;
  const auto single = find("G-single", is({Edge::rw}), is({Edge::ww, Edge::wr, Edge::order}), "a cycle with one anti-dependency");
  if (level == Isolation::snapshot) return verdict;
  // A cycle through an anti-dependency where G-single found none has at least two.
  find("G2", is({Edge::rw}), [](Edge) { return true; }, "a cycle with two or more anti-dependencies", single);
  return verdict;
}

}  // namespace dannan::test::sim
