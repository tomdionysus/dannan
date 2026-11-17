//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/history.h"

// The transaction checker by isolation level (TODO/2026-11-15-simulation-harness.md, Kvasir's
// review): cycles in Adya's dependency graph, read off the history, not searched for.
//
// Transactions are over lists, as Elle's are (Kingsbury and Alvaro, 2020): a write appends an
// element unique to it, a read returns the whole list. Every element names its writer, and the
// longest read of a key gives that key's version order, so the write-write, write-read and
// read-write edges are all read off. A register's overwrite hides its order; a list does not.
//
// A transaction is one operation of the history: its input the encoded statements, its output
// the encoded reads (encode_statements, encode_reads). Its search is compiled once, in
// dannan_sim_checkers.
namespace dannan::test::sim {

struct Statement {
  enum class Kind : uint8_t { read, append };
  Kind kind = Kind::read;
  std::string key;
  // An append's element, unique to it across the history.
  uint64_t element = 0;
  // A read's result, once answered: the list, oldest first.
  std::vector<uint64_t> list;
};

Bytes encode_statements(const std::vector<Statement>& statements);
// The reads' results, in statement order (appends take no space).
Bytes encode_reads(const std::vector<Statement>& statements);
// The statements of a transaction from its operation: its reads' lists filled in if it was
// answered. Throws std::invalid_argument for bytes that are not an encoding.
std::vector<Statement> decode(const Operation& operation);

enum class Isolation : uint8_t {
  // No G0 or G1 (a, b and c).
  read_committed,
  // No G0, G1 or G-single; G2 (write skew) is allowed.
  snapshot,
  // No cycle at all.
  serializable,
};

// Which order between transactions is part of the graph besides their dependencies.
enum class Order : uint8_t {
  none,
  // A client sees its own earlier commits.
  session,
  // A transaction answered before another was invoked comes first (strict).
  real_time,
};

struct Anomaly {
  // "G0", "G1a", "G1b", "G1c", "G-single", "G2", "incompatible-order", "internal",
  // "duplicate-element", "garbage" (an element no one appended).
  std::string kind;
  // The transactions (operation indices), and for a cycle the edges between them in turn,
  // each "ww", "wr", "rw" or the order's "session" or "real-time".
  std::vector<size_t> transactions;
  std::vector<std::string> edges;
  std::string why;
};

struct IsolationVerdict {
  bool ok = true;
  // Every anomaly the level forbids, first found first.
  std::vector<Anomaly> anomalies;
};

// Whether the history's transactions keep `level`, with `order` part of the graph. A
// transaction never answered, or answered provisionally and unresolved, counts as committed only
// if something it appended was read; one cancelled or resolved as not taken counts as aborted.
IsolationVerdict check_isolation(const History& history, Isolation level, Order order = Order::none);

}  // namespace dannan::test::sim
