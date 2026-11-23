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

## Answers (2026-11-23) and the shape they make

Louhi, Mimir and Kvasir confirmed the drawing; Lugh's answer is awaited. Their asks, merged:

- **A default configuration file** (Louhi): `ProgramSpec::default_config`, read when it exists
  and no file is named (/etc/louhi/config.yaml, LOUHI_DEFAULT_CONFIG from the package).
- **A pre-start hook** (Mimir; needed to move at all): `prepare(Node&)`, called after the node
  is built and before it starts, where a hosted system registers its message types before the
  routes freeze. As SimNodes::prepare.
- **One-shot work instead of the node** (Louhi's --retire until item 5, Mimir's --init-state,
  Kvasir's hash-password):
  - `before_start(const Settings&, NodeConfig&) -> std::optional<int>`, run after the read and
    configure: an exit code ends the program without starting the node;
  - `commands`, a map of positional names to `int(const Settings&)`, run the same way; an
    unknown positional is refused, exit 2.
- **A run that ends by itself, and a stop the host can ask for** (Louhi's and Mimir's
  --seconds; Mimir's SHUTDOWN): `node.seconds` (0, the default: until a signal), and
  `Program::stop()`, safe from any thread and posted, starting the same drain a first signal
  does.
- **The status line** (Louhi): `on_status(void(json::object&))` adds fields to the helper's
  `node.status` line; a program may instead leave node.status_ms at 0 and keep its own timer.
- **No requirements of its own** (Mimir's throwaway cluster of one): the helper refuses no
  missing cluster.key or state; whether one is needed is the program's rule, in configure,
  which may also set them (a fresh key, a temporary state directory).
- **Two stop steps** (Kvasir's rollback before the node stops; Mimir's directory removal after
  it): `on_stop` runs after the drain and before node.stop(); `on_exit` runs after node.stop(),
  last. A drain step that has not called done by node.drain_ms is left: the helper goes on to
  on_stop and node.stop() regardless (Kvasir rolls back what is still open in on_stop).
- **Events** (Mimir: listening, founded, caught_up and stopped are a program's own): the helper
  prints nothing on stdout but `{"event":"node.status",...}` when node.status_ms is set. Every
  other line is the program's, through say().
- **Listener settings** (Kvasir): `add_listener_settings(reader, name, default_port)`, `.tls.ca`
  optional. Louhi renames api.* onto it, Kvasir listen.* and tls.* onto `mysql`, each with
  cluster.* in the minor it adopts the helper.
- Kvasir moves onto the helper when it joins the cluster, not at 0.12.0's tag.

## Lugh's answer (2026-11-23): the shape is complete

Lugh confirmed (P8.1: settings, listener paths for HTTPS, LDAP, LDAPS and the OAuth mTLS port, each
by its own prefix; the drain; on_stop for its hashing and blocking pools; custody; line output;
the --version form, which its package and process tests will read). Two additions:
- **Reload on SIGHUP** (lugh.service's ExecReload=kill -HUP, a certificate renewal's hook): SIGHUP
  is never a stop. `Program::on_reload(step)` runs each step posted to the io context, and the
  helper's own listeners reread their CertificatePaths.
- **Command words** (as Kvasir's): `lugh-node init [--passphrase]` until founding (item 4) comes.
  `ProgramSpec::commands` names each, run with the settings read and before any node starts; an
  unknown word exits 2. When founding lands, Lugh's init becomes its hook.

All four consumers have answered; code starts on this shape.
