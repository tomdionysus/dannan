# The node-program helper (item 14, 0.12.0): the shape, for confirmation

Proposed 2026-11-23 to Lugh (P8.1), Mimir, Kvasir and Louhi. Code starts once each has
answered; what changes is recorded at the end.

## What it is

What `tools/dannan_node.cpp`'s main does today, as a library call, so a node binary's main is
the helper plus its hosted root (Lugh's proof: lugh_node.cpp's main is the helper and Lugh's
root). dannan-node moves onto it.

```cpp
namespace dannan::supervise {

struct ProgramSpec {
  std::string name;     // "lugh-node": messages, --version, the log scope
  std::string prefix;   // "LUGH": LUGH_<KEY> variables, LUGH_CONFIG for the file
  std::string version;  // its own version, printed by --version and put in NodeConfig::version
  // The hosted system's own settings, added to the same reader beside cluster.* and log.*.
  std::function<void(config::Reader&)> settings;
  // Its rules over what was read, after Dannan's own; throw std::runtime_error to refuse (exit 2).
  std::function<void(const config::Settings&, NodeConfig&)> configure;
};

// What the hosted root is given once the node has started.
class Program {
 public:
  boost::asio::io_context& io();          // the helper's; a second listener runs on it
  Node& node();
  Context& context();
  const config::Settings& settings() const;
  status::ScopedLogger logger(std::string scope) const;
  // One JSON object per line on stdout, whole lines from any thread ("line output").
  void say(const json::object& event);
  // A step the drain waits for, given the drain's deadline: Kvasir's open transactions, Lugh's
  // sessions. Called in the order added, each posted to the io context.
  void on_drain(std::function<void(contract::Deadline, std::function<void()> done)> step);
  // Called once, after the drain and before the node stops.
  void on_stop(std::function<void()> step);
};

// Parses argv through one reader (defaults, the file, secrets.yaml, the environment, the flags),
// then: --version prints "<name> <version> (dannan x.y.z)" and exits 0; --help lists every
// setting and exits 0; --init-custody and --show-identity do what dannan-node's do and exit.
// Otherwise starts io threads, the node, then `host`, and runs until SIGTERM or SIGINT. The
// first signal drains: readiness.drain(true), every on_drain step within
// node.drain_ms (default 30 s), on_stop, node.stop(), exit 0. A second signal stops at once,
// exit 1. A bad setting exits 2, a failure to start exits 1, each with one line on stderr.
int run_node_program(int argc, char** argv, ProgramSpec spec, std::function<void(Program&)> host);

}  // namespace dannan::supervise
```

## Settings it adds beside cluster.*

| Key | Values |
|---|---|
| `log.level` | debug, info, warn, error, all (the stack's shared key) |
| `log.format` | text or json |
| `node.io_threads` | io threads; default the cores, at least 2 |
| `node.drain_ms` | how long the drain waits for the hosted steps; default 30000 |
| `node.status_ms` | 0 (default) or the interval of a `{"event":"status",...}` line |

Flags and variables follow docs/config.md's rule, under the program's prefix.

## A second listener

The hosted root builds its own `listen::Listener` on `program.io()`. For a TLS listener's
certificate paths the helper offers `add_listener_settings(reader, "http")` (keys
`http.listen.address`, `.port`, `http.tls.certificate`, `.key`, `.ca`) and
`listener_paths(settings, "http")` returning `listen::CertificatePaths`, so each program names
its listener's settings the same way. Kvasir's MySQL listener with start_tls in place is the
listener's own, as today.

## Not yet

- `--retire` comes with item 5 (retire as one cluster act, third minor), and founding (init) with
  item 4. Until then the helper refuses neither: they are not flags yet.

## Questions to each consumer

- **Lugh:** P8.1's list (settings beside the standard ones, a public HTTPS listener's
  certificate paths, signals and drain, --version, custody, LogFormat): anything missing?
- **Kvasir:** your settings through the same reader, and the MySQL listener on program.io(),
  with your drain as an on_drain step: right?
- **Louhi:** does this replace louhi_node.cpp's scaffolding?
- **Mimir:** anything your node binary needs that this lacks?
