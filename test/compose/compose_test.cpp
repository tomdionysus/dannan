//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The compose test bed (wire v1, section 7): level 0 through real NATs.
// Builds the image, brings up test/compose/compose.yaml (two public relaying
// hubs; a1 and d1 behind endpoint-independent NATs, b1 behind a
// port-randomising one, c1 behind two), and asserts from each node's JSON
// status lines, phase by phase:
//
//   1. one cluster, and every node's path class;
//   2. every pair's path (a1 and d1 punched, other NATed pairs relayed,
//      anything with a hub ordinary) and echo on both lanes;
//   3. a partition heals: router-d stops, d1 drops out, router-d starts,
//      a1 and d1 punch again;
//   4. a relay fails over: hub1 stops, the relayed pairs move to hub2, hub1
//      returns.
//
// And the same bed as a cluster of certificates alone (wire version 2;
// compose.certificates.yaml laid over it): the host's dannan-node and
// dannan-ca make every node's custody, the CA and the certificates first,
// as an operator would; phases 1 and 2 as above; then c1 is revoked,
// handed to hub1 at a restart, and every other node drops it and keeps it
// out (TODO/2026-11-04-identity.md, section 3).
//
// Its own binary, dannan_compose_tests, built with
// -DDANNAN_BUILD_COMPOSE_TESTS=ON and run by hand with Docker running;
// never in the per-commit gate (Tom, 2026-11-04: GoogleTest, opt-in).

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/temp_dir.h"

namespace dannan::test {
namespace {

using namespace std::chrono_literals;

// --- Just enough JSON to read dannan-node's status lines ------------------

struct Json {
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value;

  const Json* at(const std::string& key) const {
    const auto* object = std::get_if<Object>(&value);
    if (!object) return nullptr;
    const auto found = object->find(key);
    return found == object->end() ? nullptr : &found->second;
  }
  std::string text(const std::string& key) const {
    const auto* found = at(key);
    const auto* s = found ? std::get_if<std::string>(&found->value) : nullptr;
    return s ? *s : std::string();
  }
  const Array& items(const std::string& key) const {
    static const Array none;
    const auto* found = at(key);
    const auto* a = found ? std::get_if<Array>(&found->value) : nullptr;
    return a ? *a : none;
  }
};

class JsonReader {
 public:
  explicit JsonReader(const std::string& text) : _text(text) {}
  std::optional<Json> read() {
    try {
      auto out = value();
      skip();
      if (_at != _text.size()) return std::nullopt;
      return out;
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }

 private:
  void skip() {
    while (_at < _text.size() && std::isspace(static_cast<unsigned char>(_text[_at]))) ++_at;
  }
  char peek() {
    skip();
    if (_at >= _text.size()) throw std::runtime_error("end");
    return _text[_at];
  }
  void expect(char c) {
    if (peek() != c) throw std::runtime_error("expected");
    ++_at;
  }
  Json value() {
    const char c = peek();
    if (c == '{') return object();
    if (c == '[') return array();
    if (c == '"') return Json{string()};
    if (_text.compare(_at, 4, "true") == 0) return _at += 4, Json{true};
    if (_text.compare(_at, 5, "false") == 0) return _at += 5, Json{false};
    if (_text.compare(_at, 4, "null") == 0) return _at += 4, Json{nullptr};
    size_t used = 0;
    const double number = std::stod(_text.substr(_at), &used);
    _at += used;
    return Json{number};
  }
  Json object() {
    expect('{');
    Json::Object out;
    if (peek() == '}') return ++_at, Json{out};
    for (;;) {
      auto key = string();
      expect(':');
      out.emplace(std::move(key), value());
      if (peek() == ',') {
        ++_at;
        continue;
      }
      expect('}');
      return Json{out};
    }
  }
  Json array() {
    expect('[');
    Json::Array out;
    if (peek() == ']') return ++_at, Json{out};
    for (;;) {
      out.push_back(value());
      if (peek() == ',') {
        ++_at;
        continue;
      }
      expect(']');
      return Json{out};
    }
  }
  std::string string() {
    expect('"');
    std::string out;
    while (_at < _text.size() && _text[_at] != '"') {
      if (_text[_at] == '\\' && _at + 1 < _text.size()) {
        const char e = _text[++_at];
        if (e == 'u' && _at + 4 < _text.size()) {
          out += static_cast<char>(std::stoi(_text.substr(_at + 1, 4), nullptr, 16));
          _at += 4;
        } else {
          out += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
        }
      } else {
        out += _text[_at];
      }
      ++_at;
    }
    expect('"');
    return out;
  }

  const std::string& _text;
  size_t _at = 0;
};

// --- The bed --------------------------------------------------------------

const std::vector<std::string> hubs{"hub1", "hub2"};
const std::vector<std::string> nated{"a1", "d1", "b1", "c1"};
const std::map<std::string, std::string> classes{{"hub1", "public"},  {"hub2", "public"},   {"a1", "punchable"},
                                                 {"d1", "punchable"}, {"b1", "relay_only"}, {"c1", "relay_only"}};

bool is_hub(const std::string& node) { return node == "hub1" || node == "hub2"; }
bool is_punchable(const std::string& node) { return node == "a1" || node == "d1"; }

std::string expected_path(const std::string& a, const std::string& b) {
  if (is_hub(a) || is_hub(b)) return "ordinary";
  if (is_punchable(a) && is_punchable(b)) return "punched";
  return "relayed";
}

class Bed {
 public:
  // `overlays`: compose files laid over compose.yaml; `environment`:
  // "NAME=value" for docker compose's interpolation.
  explicit Bed(std::vector<std::string> overlays = {}, std::vector<std::string> environment = {}) : _environment(std::move(environment)) {
    _files.push_back(std::string(DANNAN_COMPOSE_DIR) + "/compose.yaml");
    for (const auto& overlay : overlays) _files.push_back(std::string(DANNAN_COMPOSE_DIR) + "/" + overlay);
  }
  ~Bed() {
    if (!std::getenv("DANNAN_COMPOSE_KEEP")) (void)compose({"down", "-v", "--remove-orphans"}, 120s);
  }

  // docker compose with these arguments; its exit status, and what it
  // printed.
  std::pair<std::optional<int>, std::vector<std::string>> compose(std::vector<std::string> args, std::chrono::seconds within) {
    std::vector<std::string> argv{"/usr/bin/env"};
    argv.insert(argv.end(), _environment.begin(), _environment.end());
    argv.insert(argv.end(), {"docker", "compose"});
    for (const auto& file : _files) argv.insert(argv.end(), {"-f", file});
    argv.insert(argv.end(), args.begin(), args.end());
    const auto log = _dir.path() / "compose.log";
    std::vector<std::string> lines;
    std::optional<int> status;
    {
      ChildProcess child(argv, log);
      const auto deadline = std::chrono::steady_clock::now() + within;
      while (auto line = child.line(deadline)) lines.push_back(*line);
      status = child.wait(deadline);
    }
    // A failure explains itself: the tail of what docker said on stderr.
    if (status != 0) {
      std::ifstream input(log);
      std::vector<std::string> said;
      for (std::string line; std::getline(input, line);) said.push_back(line);
      const auto from = said.size() > 30 ? said.size() - 30 : 0;
      std::string tail;
      for (size_t i = from; i < said.size(); ++i) tail += "    " + said[i] + "\n";
      _failures += "docker compose " + args.front() + " exited " + (status ? std::to_string(*status) : std::string("late")) + ":\n" + tail;
    }
    return {status, lines};
  }

  // The node's latest status line.
  std::optional<Json> status(const std::string& node) {
    const auto [_, lines] = compose({"logs", "--no-log-prefix", "--tail", "40", node}, 30s);
    std::optional<Json> latest;
    for (const auto& line : lines) {
      if (line.empty() || line.front() != '{') continue;
      auto json = JsonReader(line).read();
      if (json && json->text("event") == "status") latest = std::move(json);
    }
    return latest;
  }

  // What every failed docker command said, for an assertion's message.
  const std::string& failures() const noexcept { return _failures; }

  std::map<std::string, std::optional<Json>> snapshot(const std::vector<std::string>& nodes) {
    std::map<std::string, std::optional<Json>> out;
    for (const auto& node : nodes) out[node] = status(node);
    return out;
  }

 private:
  TempDir _dir;
  std::vector<std::string> _files;
  const std::vector<std::string> _environment;
  std::string _failures;
};

// A node's view of its peers, by name.
std::map<std::string, const Json*> peers(const Json& record, const std::map<std::string, std::string>& names) {
  std::map<std::string, const Json*> out;
  for (const auto& peer : record.items("peers")) {
    const auto* inner = peer.at("record");
    if (!inner) continue;
    if (const auto name = names.find(inner->text("id")); name != names.end()) out[name->second] = &peer;
  }
  return out;
}

std::map<std::string, std::string> names_of(const std::map<std::string, std::optional<Json>>& statuses) {
  std::map<std::string, std::string> out;
  for (const auto& [node, record] : statuses)
    if (record && record->at("self")) out[record->at("self")->text("id")] = node;
  return out;
}

// What is not yet as expected; empty when all is.
std::vector<std::string> problems(const std::map<std::string, std::optional<Json>>& statuses, const std::vector<std::string>& nodes, bool pairs) {
  std::vector<std::string> out;
  const auto names = names_of(statuses);
  for (const auto& node : nodes) {
    const auto found = statuses.find(node);
    if (found == statuses.end() || !found->second || !found->second->at("self")) {
      out.push_back(node + ": no status yet");
      continue;
    }
    const auto& record = *found->second;
    const auto path_class = record.at("self")->text("path_class");
    if (path_class != classes.at(node)) out.push_back(node + ": path class " + path_class + ", expected " + classes.at(node));
    const auto seen = peers(record, names);
    for (const auto& other : nodes) {
      if (other == node) continue;
      const auto peer = seen.find(other);
      if (peer == seen.end()) {
        out.push_back(node + ": does not list " + other);
        continue;
      }
      if (!pairs) continue;
      for (const auto* lane : {"control", "data"}) {
        const auto* echo = peer->second->at("echo");
        const auto verdict = echo ? echo->text(lane) : std::string();
        if (verdict != "ok") out.push_back(node + " -> " + other + ": echo on " + lane + ": " + (verdict.empty() ? "none yet" : verdict));
      }
      // NATed nodes open their own paths; a hub's routes to them are theirs.
      if (std::find(nated.begin(), nated.end(), node) != nated.end()) {
        const auto* path = peer->second->at("path");
        const auto got = path ? path->text("control") : std::string();
        if (got != expected_path(node, other)) out.push_back(node + " -> " + other + ": path " + got + ", expected " + expected_path(node, other));
      }
    }
  }
  return out;
}

// Waits until `check` finds nothing wrong, polling every two seconds;
// what was still wrong at the deadline.
std::vector<std::string> settle(const std::function<std::vector<std::string>()>& check, std::chrono::seconds within) {
  const auto until = std::chrono::steady_clock::now() + within;
  auto last = check();
  while (!last.empty() && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(2s);
    last = check();
  }
  return last;
}

std::string joined(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) out += "  " + line + "\n";
  return out;
}

std::vector<std::string> all_nodes() {
  auto out = hubs;
  out.insert(out.end(), nated.begin(), nated.end());
  return out;
}

// Builds the image (unless DANNAN_COMPOSE_NO_BUILD), brings the bed up, and
// asserts phases 1 and 2.
void up_and_settled(Bed& bed) {
  if (!std::getenv("DANNAN_COMPOSE_NO_BUILD")) {
    const auto [built, output] = bed.compose({"build", "hub1"}, 1800s);
    ASSERT_EQ(built, 0) << "the image did not build\n" << bed.failures();
  }
  (void)bed.compose({"down", "-v", "--remove-orphans"}, 120s);
  ASSERT_EQ(bed.compose({"up", "-d", "--pull", "never"}, 300s).first, 0) << "the bed did not come up\n" << bed.failures();
  const auto nodes = all_nodes();
  auto wrong = settle([&] { return problems(bed.snapshot(nodes), nodes, false); }, 180s);
  ASSERT_TRUE(wrong.empty()) << "one cluster, path classes:\n" << joined(wrong);
  wrong = settle([&] { return problems(bed.snapshot(nodes), nodes, true); }, 180s);
  ASSERT_TRUE(wrong.empty()) << "every pair's path and echo on both lanes:\n" << joined(wrong);
}

// Runs a host tool with `environment` added; what it printed on stdout.
// Throws if it fails or takes over two minutes.
std::string run(const std::vector<std::string>& args, const std::vector<std::string>& environment, const std::filesystem::path& log) {
  std::vector<std::string> argv{"/usr/bin/env"};
  argv.insert(argv.end(), environment.begin(), environment.end());
  argv.insert(argv.end(), args.begin(), args.end());
  ChildProcess child(argv, log);
  const auto deadline = std::chrono::steady_clock::now() + 120s;
  std::string out;
  while (auto line = child.line(deadline)) out += *line + "\n";
  if (child.wait(deadline) != 0) {
    std::ifstream input(log);
    throw std::runtime_error(args.front() + " " + args.at(1) +
                             " failed: " + std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()));
  }
  return out;
}

// The text of `"key":"value"` in one line of JSON.
std::string field(const std::string& line, const std::string& key) {
  auto json = JsonReader(line).read();
  return json ? json->text(key) : std::string();
}

TEST(ComposeBed, LevelZeroThroughRealNats) {
  Bed bed;
  up_and_settled(bed);
  if (HasFatalFailure()) return;
  const auto nodes = all_nodes();
  std::vector<std::string> wrong;

  // A partition: d1's router stops, d1 drops out; it starts, d1 comes back.
  ASSERT_EQ(bed.compose({"stop", "router-d"}, 120s).first, 0);
  std::vector<std::string> others;
  for (const auto& node : nodes)
    if (node != "d1") others.push_back(node);
  wrong = settle(
      [&] {
        auto statuses = bed.snapshot(others);
        statuses["d1"] = bed.status("d1");
        const auto names = names_of(statuses);
        std::vector<std::string> out;
        for (const auto& node : others)
          if (statuses[node] && peers(*statuses[node], names).contains("d1")) out.push_back(node + " still lists d1");
        return out;
      },
      120s);
  ASSERT_TRUE(wrong.empty()) << "d1 drops out:\n" << joined(wrong);
  ASSERT_EQ(bed.compose({"start", "router-d"}, 120s).first, 0);
  wrong = settle([&] { return problems(bed.snapshot(nodes), nodes, true); }, 180s);
  ASSERT_TRUE(wrong.empty()) << "d1 back, punched to a1, every pair answering:\n" << joined(wrong);

  // A relay fails over: hub1 stops, the relayed pairs move to hub2.
  ASSERT_EQ(bed.compose({"stop", "hub1"}, 120s).first, 0);
  std::vector<std::string> rest;
  for (const auto& node : nodes)
    if (node != "hub1") rest.push_back(node);
  wrong = settle([&] { return problems(bed.snapshot(rest), rest, true); }, 180s);
  ASSERT_TRUE(wrong.empty()) << "the relayed pairs move to hub2:\n" << joined(wrong);
  ASSERT_EQ(bed.compose({"start", "hub1"}, 120s).first, 0);
  wrong = settle([&] { return problems(bed.snapshot(nodes), nodes, true); }, 180s);
  ASSERT_TRUE(wrong.empty()) << "all six as at the start:\n" << joined(wrong);
}

// The same figures dannan-bench gives on loopback, through what the bed makes real: a
// direct dial to a hub, a punch through two NATs, a relay. Recorded, not asserted; a
// failed transfer is the only failure. Each runs from behind a1's router.
TEST(ComposeBed, MeasuresThroughNatsAndARelay) {
  Bed bed;
  up_and_settled(bed);
  if (HasFatalFailure()) return;
  for (const auto* peer : {"hub1", "d1", "b1"}) {
    const auto [status, lines] = bed.compose(
        {"exec", "-T", "-w", "/tmp", "a1", "sh", "-c",
         std::string("/build/dannan-bench --megabytes 32 --rounds 1 --bootstrap 10.10.0.10:7437 --advertise $ADVERTISE --key /tmp/cluster.key --peer-name ") +
             peer},
        600s);
    EXPECT_EQ(status, 0) << peer;
    for (const auto& line : lines) std::cout << peer << ": " << line << std::endl;
  }
}

TEST(ComposeBed, CertificatesAloneThroughRealNats) {
  // Under the build tree, which Docker shares; the CA's own key stays out
  // of what the nodes are given.
  TempDir identities;
  TempDir authority;
  const std::string passphrase = "DANNAN_CUSTODY_PASSPHRASE=dannan compose bed custody";
  const auto log = authority.path() / "tools.log";
  run({DANNAN_CA_BINARY, "init", "--dir", (authority.path() / "ca").string(), "--name", "dannan-bed", "--passphrase-env", "DANNAN_CUSTODY_PASSPHRASE"},
      {passphrase}, log);
  std::filesystem::copy_file(authority.path() / "ca" / "ca.pem", identities.path() / "ca.pem");
  std::map<std::string, std::string> ids;
  for (const auto& node : all_nodes()) {
    const auto state = (identities.path() / node).string();
    run({DANNAN_NODE_BINARY, "--state", state, "--init-custody", "--custody-passphrase-env", "DANNAN_CUSTODY_PASSPHRASE"}, {passphrase}, log);
    const auto shown =
        run({DANNAN_NODE_BINARY, "--state", state, "--show-identity", "--custody-passphrase-env", "DANNAN_CUSTODY_PASSPHRASE"}, {passphrase}, log);
    ids[node] = field(shown, "id");
    ASSERT_FALSE(ids[node].empty()) << shown;
    const auto pem = run({DANNAN_CA_BINARY, "issue", "--dir", (authority.path() / "ca").string(), "--passphrase-env", "DANNAN_CUSTODY_PASSPHRASE", "--node",
                          ids[node], "--public-key", field(shown, "public_key")},
                         {passphrase}, log);
    std::ofstream(identities.path() / node / "node.pem") << pem;
  }

  Bed bed({"compose.certificates.yaml"}, {passphrase, "DANNAN_IDENTITY_DIR=" + identities.path().string()});
  up_and_settled(bed);
  if (HasFatalFailure()) return;

  // c1 revoked: the revocation handed to hub1 at a restart reaches every
  // node, and none lists c1 again.
  run({DANNAN_CA_BINARY, "revoke", "--dir", (authority.path() / "ca").string(), "--passphrase-env", "DANNAN_CUSTODY_PASSPHRASE", "--node", ids["c1"],
       "--reason", "compose bed", "--out", (identities.path() / "revocations").string()},
      {passphrase}, log);
  ASSERT_EQ(bed.compose({"restart", "hub1"}, 120s).first, 0) << bed.failures();
  std::vector<std::string> rest;
  for (const auto& node : all_nodes())
    if (node != "c1") rest.push_back(node);
  const auto still_listing_c1 = [&] {
    auto statuses = bed.snapshot(rest);
    statuses["c1"] = bed.status("c1");
    const auto names = names_of(statuses);
    std::vector<std::string> out;
    for (const auto& node : rest)
      if (!statuses[node])
        out.push_back(node + ": no status");
      else if (peers(*statuses[node], names).contains("c1"))
        out.push_back(node + " still lists c1");
    return out;
  };
  auto wrong = settle(still_listing_c1, 180s);
  ASSERT_TRUE(wrong.empty()) << "c1 dropped once revoked:\n" << joined(wrong);
  std::this_thread::sleep_for(20s);
  wrong = still_listing_c1();
  ASSERT_TRUE(wrong.empty()) << "c1 kept out:\n" << joined(wrong);
  wrong = settle([&] { return problems(bed.snapshot(rest), rest, true); }, 180s);
  ASSERT_TRUE(wrong.empty()) << "the other five as before:\n" << joined(wrong);
}

}  // namespace
}  // namespace dannan::test
