//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

/// @file
/// A node binary's main, as a library call: settings, io threads, the node, signals and the drain.

#include <boost/asio/io_context.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "dannan/config/config.h"
#include "dannan/contract/completion.h"
#include "dannan/json/json.h"
#include "dannan/listen/certificates.h"
#include "dannan/node.h"

namespace dannan::supervise {

class Program;

/// What a node binary is (0.12.0; TODO/2026-11-23-node-program.md, agreed with Lugh, Mimir, Kvasir
/// and Louhi). Every hook is optional.
struct ProgramSpec {
  /// "lugh-node": in messages, --version and the log scope.
  std::string name;
  /// "LUGH": the prefix of every variable (LUGH_CLUSTER_LISTEN_PORT), and LUGH_CONFIG names the file.
  std::string prefix;
  /// Its own version: what --version prints, and NodeConfig::version.
  std::string version;
  /// Read when no file is named and it exists (/etc/louhi/config.yaml); empty: none.
  std::string default_config;
  /// The program's own settings, added to the reader beside cluster.*, log.* and node.*.
  std::function<void(config::Reader&)> settings;
  /// Its rules over what was read, after Dannan's. The helper requires no key or state of its own,
  /// so this may set them itself (a throwaway cluster of one). Throw std::runtime_error (or
  /// config::ConfigError) to refuse: exit 2.
  std::function<void(const config::Settings&, NodeConfig&)> configure;
  /// One-shot work after the read and configure, before any node is built (--retire,
  /// --init-state): an exit code ends the program there, and no node starts.
  std::function<std::optional<int>(const config::Settings&, NodeConfig&)> before_start;
  /// Command words (`lugh-node init`, `kvasir-node hash-password`), run with the settings read and
  /// before any node starts; the result is the exit code. A positional word not named here exits 2.
  std::map<std::string, std::function<int(const config::Settings&)>> commands;
  /// Called on the built node before it starts: where a hosted system registers its message types,
  /// before the routes freeze.
  std::function<void(Node&)> prepare;
};

/// What the hosted root is given once its node has started. Every member is thread-safe.
class Program {
 public:
  virtual ~Program() = default;
  /// The helper's io context: a second listener (HTTPS, MySQL, RESP) runs on it.
  virtual boost::asio::io_context& io() = 0;
  /// The node, started.
  virtual Node& node() = 0;
  /// The node's Context.
  virtual Context& context() = 0;
  /// Every setting read, the program's own included.
  virtual const config::Settings& settings() const = 0;
  /// A logger on the program's sink (log.level, log.format) under `scope`.
  virtual status::ScopedLogger logger(std::string scope) const = 0;
  /// Writes one JSON object as one line on stdout: whole lines from any thread. Every line but the
  /// helper's node.status is the program's own (Mimir's listening, founded, stopped).
  virtual void say(const json::object& event) = 0;
  /// A step the drain waits for, given the drain's deadline: it calls `done` once its work has
  /// ended (Kvasir's open transactions, Lugh's sessions). Posted to the io context, in the order
  /// added. A step that has not called done by node.drain_ms is left, and the stop goes on.
  virtual void on_drain(std::function<void(contract::Deadline, std::function<void()> done)> step) = 0;
  /// After the drain, before the node stops (Kvasir's rollback, Lugh's pools), in the order added.
  virtual void on_stop(std::function<void()> step) = 0;
  /// After the node has stopped, last (Mimir's temporary state removed), in the order added.
  virtual void on_exit(std::function<void()> step) = 0;
  /// On SIGHUP, posted to the io context (Lugh: a certificate renewal reloads every listener's
  /// pairs without a restart). SIGHUP never stops the program.
  virtual void on_reload(std::function<void()> step) = 0;
  /// Adds fields to each node.status line, when node.status_ms is set (Louhi's tasks and passes).
  virtual void on_status(std::function<void(json::object&)> step) = 0;
  /// Begins the drain a first signal begins (Mimir's SHUTDOWN). Posted; waits on nothing.
  virtual void stop() = 0;
};

/// Runs a node binary.
///
/// Parses `argv` through one reader: defaults, the file (--config, -c, PREFIX_CONFIG or the
/// default), secrets.yaml beside it, the environment, then the flags. Then:
///   - `--help` lists every setting, exit 0; `--version` prints "<name> <version> (dannan x.y.z)",
///     exit 0;
///   - a command word runs its command;
///   - `--init-custody` makes the node's key custody and prints its root; `--show-identity` prints
///     the node's id and public key; each exits;
///   - otherwise configure, before_start, then io threads (node.io_threads), the node, prepare,
///     start, the revocations file applied, and `host`, running until SIGTERM, SIGINT,
///     Program::stop() or node.seconds.
///
/// The first stop drains: readiness.drain(true), every on_drain step within node.drain_ms, then
/// on_stop, the node's stop, on_exit, exit 0. A second signal stops at once, exit 1. SIGHUP runs
/// the on_reload steps. A setting that cannot be used exits 2, a failure to start 1, each with one
/// line on stderr. The helper prints nothing on stdout but `{"event":"node.status",...}` lines,
/// when node.status_ms is set.
///
/// Its settings beside the program's: cluster.* (config::add_node_settings), log.level,
/// log.format, node.io_threads, node.drain_ms, node.status_ms and node.seconds.
///
/// @code
/// int main(int argc, char** argv) {
///   dannan::supervise::ProgramSpec spec;
///   spec.name = "lugh-node";
///   spec.prefix = "LUGH";
///   spec.version = "0.7.0";
///   spec.settings = [](dannan::config::Reader& reader) {
///     dannan::supervise::add_listener_settings(reader, "LUGH", "https", 443);
///   };
///   return dannan::supervise::run_node_program(argc, argv, spec, [](dannan::supervise::Program& program) {
///     auto https = dannan::supervise::listener_settings(program.settings(), "https", 443);
///     // build the hosted root on program.context() and its listener on program.io()
///     program.on_drain([](auto deadline, auto done) { /* finish open sessions */ done(); });
///   });
/// }
/// @endcode
/// @return the process's exit code.
int run_node_program(int argc, char** argv, ProgramSpec spec, std::function<void(Program&)> host);

/// Adds a listener's settings under `name`, so each program names its listeners alike:
/// name.listen.address, name.listen.port, name.tls.certificate, name.tls.key and name.tls.ca, with
/// their flags and variables under `prefix`.
/// @param default_port the port when none is given, as listener_settings() is told too.
void add_listener_settings(config::Reader& reader, const std::string& prefix, const std::string& name, uint16_t default_port);

/// A listener's settings, as read.
struct ListenerSettings {
  /// The address to listen on; empty: the listener's own default.
  std::string address;
  /// The port.
  uint16_t port = 0;
  /// The certificate paths, set when a certificate and its key are both given; the ca is the
  /// client bundle, optional (MySQL client certificates are optional, Kvasir).
  std::optional<listen::CertificatePaths> tls;
};
/// Reads a listener's settings under `name`.
/// @throws config::ConfigError for a port out of range, a certificate without its key or a key
///   without its certificate, or a ca without either.
ListenerSettings listener_settings(const config::Settings& settings, const std::string& name, uint16_t default_port);

}  // namespace dannan::supervise
