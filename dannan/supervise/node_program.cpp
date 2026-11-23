//
// Dannan - The Distributed Substrate
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "dannan/supervise/node_program.h"

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cctype>
#include <cstdio>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "dannan/config/node_settings.h"
#include "dannan/version.h"

namespace dannan::supervise {

namespace asio = boost::asio;

namespace {

// docs/config.md's rule: the key's flag and its variable under the prefix.
void add_setting(config::Reader& reader, const std::string& prefix, std::string key, config::Setting::Kind kind, std::string what,
                 bool from_environment = true) {
  std::string flag = "--";
  std::string variable = prefix + "_";
  for (const char c : key) {
    flag += c == '.' || c == '_' ? '-' : c;
    variable += c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  reader.settings.push_back({std::move(key), kind, std::move(flag), from_environment ? std::move(variable) : std::string(), std::move(what)});
}

class Running final : public Program {
 public:
  Running(asio::io_context& io, Node& node, const config::Settings& settings, std::shared_ptr<status::LogSink> log, std::string name)
      : _io(io), _node(node), _settings(settings), _log(std::move(log)), _name(std::move(name)) {}

  asio::io_context& io() override { return _io; }
  Node& node() override { return _node; }
  Context& context() override { return _node.context(); }
  const config::Settings& settings() const override { return _settings; }
  status::ScopedLogger logger(std::string scope) const override { return {_log, std::move(scope)}; }

  void say(const json::object& event) override {
    const auto bytes = json::to_bytes(json::value(event));
    std::lock_guard lock(_out);
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
  }

  void on_drain(std::function<void(contract::Deadline, std::function<void()>)> step) override {
    std::lock_guard lock(_steps);
    _drains.push_back(std::move(step));
  }
  void on_stop(std::function<void()> step) override {
    std::lock_guard lock(_steps);
    _stops.push_back(std::move(step));
  }
  void on_exit(std::function<void()> step) override {
    std::lock_guard lock(_steps);
    _exits.push_back(std::move(step));
  }
  void on_reload(std::function<void()> step) override {
    std::lock_guard lock(_steps);
    _reloads.push_back(std::move(step));
  }
  void on_status(std::function<void(json::object&)> step) override {
    std::lock_guard lock(_steps);
    _statuses.push_back(std::move(step));
  }

  void stop() override {
    asio::post(_io, [this] { begin_drain(); });
  }

  // A second stop while draining ends it at once, exit 1.
  void signalled() {
    if (_draining.load()) return hard_stop();
    begin_drain();
  }

  void reload() {
    std::vector<std::function<void()>> steps;
    {
      std::lock_guard lock(_steps);
      steps = _reloads;
    }
    status::ScopedLogger(_log, _name).info("reloading on SIGHUP");
    for (auto& step : steps) asio::post(_io, [step] { step(); });
  }

  void status_line() {
    json::object line;
    line["event"] = "node.status";
    line["id"] = identity::to_string(_node.context().identity().id());
    line["health"] = status::health_name(_node.readiness().health());
    line["peers"] = static_cast<uint64_t>(_node.context().nodes().active().size());
    std::vector<std::function<void(json::object&)>> steps;
    {
      std::lock_guard lock(_steps);
      steps = _statuses;
    }
    for (auto& step : steps) step(line);
    say(line);
  }

  // Waits for the drain's end; the exit code.
  int wait() { return _finished.get_future().get(); }

  std::vector<std::function<void()>> stops() {
    std::lock_guard lock(_steps);
    return _stops;
  }
  std::vector<std::function<void()>> exits() {
    std::lock_guard lock(_steps);
    return _exits;
  }

  contract::Deadline drain_for{std::chrono::seconds(30)};

 private:
  // On the io context.
  void begin_drain() {
    if (_draining.exchange(true)) return;
    _node.readiness().drain(true, "stopping");
    std::vector<std::function<void(contract::Deadline, std::function<void()>)>> steps;
    {
      std::lock_guard lock(_steps);
      steps = _drains;
    }
    if (steps.empty()) return finish(0);
    auto left = std::make_shared<size_t>(steps.size());
    auto timer = std::make_shared<asio::steady_timer>(_io);
    timer->expires_after(drain_for);
    // The handler holds the timer: a step that never calls done drops its copy, and the deadline
    // must still fire.
    timer->async_wait([this, timer](const boost::system::error_code& error) {
      if (error) return;
      status::ScopedLogger(_log, _name).warn("the drain did not finish within " + std::to_string(drain_for.count()) + " ms: stopping regardless");
      finish(0);
    });
    for (auto& step : steps) {
      asio::post(_io, [this, step, left, timer] {
        step(drain_for, [this, left, timer] {
          asio::post(_io, [this, left, timer] {
            if (--*left == 0) {
              timer->cancel();
              finish(0);
            }
          });
        });
      });
    }
  }

  void hard_stop() {
    status::ScopedLogger(_log, _name).warn("a second signal: stopping at once");
    finish(1);
  }

  void finish(int code) {
    if (_finished_set.exchange(true)) return;
    _finished.set_value(code);
  }

  asio::io_context& _io;
  Node& _node;
  const config::Settings& _settings;
  const std::shared_ptr<status::LogSink> _log;
  const std::string _name;
  std::mutex _out;
  std::mutex _steps;
  std::vector<std::function<void(contract::Deadline, std::function<void()>)>> _drains;
  std::vector<std::function<void()>> _stops;
  std::vector<std::function<void()>> _exits;
  std::vector<std::function<void()>> _reloads;
  std::vector<std::function<void(json::object&)>> _statuses;
  // Set from any io thread: a signal, a stop, the drain's deadline and its last step may race.
  std::atomic<bool> _draining{false};
  std::atomic<bool> _finished_set{false};
  std::promise<int> _finished;
};

int refuse(const std::string& name, const std::string& why, int code) {
  std::cerr << name << ": " << why << "\n";
  return code;
}

}  // namespace

void add_listener_settings(config::Reader& reader, const std::string& prefix, const std::string& name, uint16_t default_port) {
  using Kind = config::Setting::Kind;
  add_setting(reader, prefix, name + ".listen.address", Kind::value, "the address " + name + " listens on");
  add_setting(reader, prefix, name + ".listen.port", Kind::value,
              "the port " + name + " listens on, 0 to 65535 (default " + std::to_string(default_port) + ")");
  add_setting(reader, prefix, name + ".tls.certificate", Kind::value, "a PEM chain, leaf first, for " + name + "'s TLS");
  add_setting(reader, prefix, name + ".tls.key", Kind::value, "the PEM key of " + name + "'s certificate");
  add_setting(reader, prefix, name + ".tls.ca", Kind::value, "a PEM bundle " + name + " checks client certificates against");
}

ListenerSettings listener_settings(const config::Settings& settings, const std::string& name, uint16_t default_port) {
  ListenerSettings out;
  out.address = settings.text(name + ".listen.address").value_or("");
  out.port = static_cast<uint16_t>(settings.number(name + ".listen.port", 0, 65535).value_or(default_port));
  const auto certificate = settings.path_of(name + ".tls.certificate");
  const auto key = settings.path_of(name + ".tls.key");
  const auto ca = settings.path_of(name + ".tls.ca");
  if (certificate.has_value() != key.has_value())
    throw settings.refuse(certificate ? name + ".tls.certificate" : name + ".tls.key", "given with its pair (a certificate and its key together)");
  if (ca && !certificate) throw settings.refuse(name + ".tls.ca", "given with a certificate and key");
  if (certificate) out.tls = listen::CertificatePaths{*certificate, *key, ca.value_or("")};
  return out;
}

int run_node_program(int argc, char** argv, ProgramSpec spec, std::function<void(Program&)> host) {
  using Kind = config::Setting::Kind;
  config::Reader reader;
  config::add_node_settings(reader, spec.prefix);
  add_setting(reader, spec.prefix, "log.level", Kind::value, "debug, info, warn, error or all");
  add_setting(reader, spec.prefix, "log.format", Kind::value, "text or json");
  add_setting(reader, spec.prefix, "node.io_threads", Kind::value, "io threads, 1 to 256 (default: the cores, at least 2)");
  add_setting(reader, spec.prefix, "node.drain_ms", Kind::value, "how long a stop waits for the drain, in ms (default 30000)");
  add_setting(reader, spec.prefix, "node.status_ms", Kind::value, "ms between node.status lines (default 0: none)");
  add_setting(reader, spec.prefix, "node.seconds", Kind::value, "seconds to run, then stop as on a signal (default 0: until one)");
  add_setting(reader, spec.prefix, "init_custody", Kind::toggle, "make the node's key custody, print its root, and exit", false);
  add_setting(reader, spec.prefix, "show_identity", Kind::toggle, "print the node's id and public key, and exit", false);
  add_setting(reader, spec.prefix, "version", Kind::toggle, "print the version, and exit", false);
  add_setting(reader, spec.prefix, "help", Kind::toggle, "list every setting, and exit", false);
  if (spec.settings) spec.settings(reader);
  reader.config_variable = spec.prefix + "_CONFIG";
  reader.default_path = spec.default_config;

  config::Settings settings;
  try {
    settings = reader.read(std::vector<std::string>(argv + 1, argv + argc));
  } catch (const std::exception& error) {
    return refuse(spec.name, error.what(), 2);
  }
  try {
    if (settings.toggle("help").value_or(false)) {
      std::cout << reader.describe();
      return 0;
    }
    if (settings.toggle("version").value_or(false)) {
      std::cout << spec.name << " " << spec.version << " (dannan " << kVersion << ")\n";
      return 0;
    }
  } catch (const std::exception& error) {
    return refuse(spec.name, error.what(), 2);
  }
  for (const auto& warning : settings.warnings) std::cerr << spec.name << ": " << warning << "\n";
  if (!settings.positional.empty()) {
    const auto found = spec.commands.find(settings.positional.front());
    if (found == spec.commands.end()) return refuse(spec.name, "unknown command " + settings.positional.front(), 2);
    try {
      return found->second(settings);
    } catch (const std::exception& error) {
      return refuse(spec.name, error.what(), 1);
    }
  }

  NodeConfig config;
  std::shared_ptr<status::LogSink> log;
  contract::Deadline drain_for{std::chrono::seconds(30)};
  std::chrono::milliseconds status_every{0};
  std::chrono::seconds run_for{0};
  size_t io_threads = std::max<size_t>(std::thread::hardware_concurrency(), 2);
  try {
    config = config::node_config(settings);
    const auto level = status::parse_log_level(settings.text("log.level").value_or("info"));
    const auto format = status::parse_log_format(settings.text("log.format").value_or("text"));
    log = status::console_sink(format, level);
    config.log = log;
    config.version = spec.version;
    if (const auto threads = settings.number("node.io_threads", 1, 256)) io_threads = static_cast<size_t>(*threads);
    if (const auto drain = settings.number("node.drain_ms", 0, 86400000)) drain_for = std::chrono::milliseconds(*drain);
    if (const auto every = settings.number("node.status_ms", 0, 86400000)) status_every = std::chrono::milliseconds(*every);
    if (const auto seconds = settings.number("node.seconds", 0, 31536000)) run_for = std::chrono::seconds(*seconds);
    if (spec.configure) spec.configure(settings, config);
  } catch (const std::exception& error) {
    return refuse(spec.name, error.what(), 2);
  }

  const bool init_custody = settings.toggle("init_custody").value_or(false);
  const bool show_identity = settings.toggle("show_identity").value_or(false);
  if (init_custody || show_identity) {
    if (config.state_path.empty() || !config.custody)
      return refuse(spec.name, "the custody commands need the state (--cluster-state) and the custody's source (--cluster-custody-...)", 2);
    try {
      if (init_custody) {
        // The root of a new credential goes to stdout and nowhere else.
        auto [custody, root] = identity::KeyCustody::create_and_open(config.state_path, *config.custody);
        (void)custody;
        if (root) std::printf("%s\n", identity::hex(root->reveal()).c_str());
        return 0;
      }
      const auto custody = identity::KeyCustody::open(config.state_path, *config.custody);
      std::printf("{\"id\":\"%s\",\"public_key\":\"%s\"}\n", identity::to_string(identity::load_or_create_node_id(config.state_path)).c_str(),
                  identity::hex(custody.node_key().public_key()).c_str());
      return 0;
    } catch (const std::exception& error) {
      return refuse(spec.name, error.what(), 1);
    }
  }
  if (spec.before_start) {
    try {
      if (const auto code = spec.before_start(settings, config)) return *code;
    } catch (const std::exception& error) {
      return refuse(spec.name, error.what(), 1);
    }
  }

  signal(SIGPIPE, SIG_IGN);
  asio::io_context io;
  auto work = asio::make_work_guard(io);
  // Set before the threads run, so no signal is missed while they start.
  asio::signal_set signals(io, SIGINT, SIGTERM, SIGHUP);
  std::vector<std::thread> threads;
  for (size_t i = 0; i < io_threads; ++i) threads.emplace_back([&io] { io.run(); });
  const auto end_threads = [&] {
    boost::system::error_code ignored;
    signals.cancel(ignored);
    work.reset();
    io.stop();
    for (auto& thread : threads) thread.join();
  };

  int code = 0;
  try {
    Node node(io, config);
    if (spec.prepare) spec.prepare(node);
    node.start();
    for (const auto& revocation : config::node_revocations(settings)) node.context().identity().revoke(revocation);
    Running running(io, node, settings, log, spec.name);
    running.drain_for = drain_for;
    std::function<void(const boost::system::error_code&, int)> on_signal = [&](const boost::system::error_code& error, int number) {
      if (error) return;
      if (number == SIGHUP)
        running.reload();
      else
        running.signalled();
      signals.async_wait(on_signal);
    };
    signals.async_wait(on_signal);
    asio::steady_timer status_timer(io);
    std::function<void(const boost::system::error_code&)> on_status = [&](const boost::system::error_code& error) {
      if (error) return;
      running.status_line();
      status_timer.expires_after(status_every);
      status_timer.async_wait(on_status);
    };
    if (status_every.count() > 0) {
      status_timer.expires_after(status_every);
      status_timer.async_wait(on_status);
    }
    asio::steady_timer end_timer(io);
    if (run_for.count() > 0) {
      end_timer.expires_after(run_for);
      end_timer.async_wait([&](const boost::system::error_code& error) {
        if (!error) running.stop();
      });
    }
    try {
      host(running);
      code = running.wait();
    } catch (const std::exception& error) {
      // The host failed to start: the node still stops in order.
      code = refuse(spec.name, error.what(), 1);
    }
    asio::post(io, [&] {
      status_timer.cancel();
      end_timer.cancel();
    });
    for (auto& step : running.stops()) step();
    node.stop();
    for (auto& step : running.exits()) step();
    end_threads();
  } catch (const std::exception& error) {
    end_threads();
    return refuse(spec.name, error.what(), 1);
  }
  return code;
}

}  // namespace dannan::supervise
