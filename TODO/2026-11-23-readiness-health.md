# Readiness and health (item 7, 0.12.0): the shape, for confirmation

Proposed 2026-11-23 to Lugh, Louhi, Macha, Kvasir and Dagda. Code starts once each has answered;
what changes is recorded at the end.

## What exists

`status::Readiness` (0.5.x on) covers startup only: planes declared before start, each marked
ready, `hold_until_marked`, `fail`, and a `Phase` (starting, recovering, ready) on telemetry. Once
complete it never falls. The clock rule (item 6, 0.10.0) stops a node issuing hybrid-clock stamps
when it is out of bounds, but nothing reads that as readiness.

## The shape

```cpp
namespace dannan::status {

// Worst wins: the order is the severity.
enum class Health : uint8_t { ready = 0, degraded = 1, draining = 2, not_ready = 3 };
const char* health_name(Health) noexcept;

struct CheckState {
  std::string name;
  Health health = Health::ready;
  std::string reason;            // empty when ready
  uint64_t since_unix_ms = 0;    // when it last changed
};

class Readiness {  // gains, beside what it has
 public:
  // A check's verdict, pushed by whoever watches the thing checked (Louhi's Docker probe, Dagda's
  // spaces caught up, Lugh's hashing backlog): never a callback Dannan runs, so a check that does
  // I/O blocks nothing of Dannan's. `valid_for`, if given, is how long the verdict stands: one
  // not set again within it reads as not_ready ("stopped reporting"). Thread-safe, waits on
  // nothing. Names under "dannan." are Dannan's own.
  void set(const std::string& check, Health health, std::string reason = {},
           std::optional<std::chrono::milliseconds> valid_for = std::nullopt);
  void clear(const std::string& check);

  // The operator's drain (--drain, or item 5's retire first): draining until undone.
  void drain(bool on, std::string reason = {});

  // The node's health: not_ready while startup is incomplete or failed, else the worst of the
  // drain and every check.
  Health health() const;
  std::vector<CheckState> checks() const;

  // Told of each change of health(), posted to `executor`, never inline.
  uint64_t on_change(asio::any_io_executor executor, std::function<void(Health)> handler);
  void cancel(uint64_t id);
};

// A peer's advertised health, from its record; a record without one (a node before 0.12.0) reads
// ready.
Health health_of(const membership::NodeRecord& node);

}  // namespace dannan::status
```

- **The advert.** The node's own record carries `dannan.health` (one byte), rewritten only when
  health() changes, never figures. A change from ready or degraded to not_ready is advertised only
  once it has held for `NodeConfig::health_hold` (default 10 s), so a flapping check does not move
  placement; draining, and every change back towards ready, are advertised at once.
- **Dannan's own checks**, run on the status step (once a second at most):
  - `dannan.memory`: degraded while the retained-memory ledger's reserve is exhausted;
  - `dannan.gates`: degraded while a work class's gate has refused for more than 5 s running;
  - `dannan.clock`: not_ready while the node is out of clock bounds (item 6's rule; Lugh's proof:
    a skewed node stops answering /ready);
  - `dannan.certificate`: degraded within 7 days of the node certificate's expiry, not_ready past
    it (item 20's expiry, on Status too).
  A hosted system's "store not ready" is its startup plane, as today, or a check of its own after
  startup.
- **Placement.** `by_capacity` and `rendezvous` demote, never remove: preferred owners come from
  nodes advertising ready or degraded while there are at least `count` of them, and draining then
  not_ready nodes follow as fallbacks, as `demote_relay_only` does. So a placement always fills
  while any node is known, and a node coming back takes its own shards back. On by default (a
  behaviour change, in the CHANGELOG); `Scheme::ignore_health` turns it off.
- **Context** exposes `readiness()` const (Macha's ask: no const_cast).
- **HTTP's /ready (0.13.0)** answers 200 for ready and degraded, 503 for draining and not_ready,
  the body naming every check that is not ready.

## Questions to each consumer

- **Dagda:** placement's demotion moves preferred ownership when a node is advertised not_ready
  or draining, after the 10 s hold. Is demotion (not removal) with that hold right for your
  owners, or do you need placement to ignore health and read it yourself?
- **Louhi:** your checks (Docker answers, memory and user namespaces enforced, storage bounds) as
  pushed verdicts with valid_for: does that fit your probe loop?
- **Lugh:** /ready as above, and your hashing backlog as a pushed check: right?
- **Kvasir:** refusing clients while health() is not_ready (not while degraded): right?
- **Macha:** `readiness()` const on Context, with health() and checks() for your Status: anything
  missing?

## Answers (2026-11-23)

- **Dagda:** confirmed. Demotion, not removal, with the 10 s hold, suits its owners and
  evacuation; no `Scheme::ignore_health` needed (kept anyway, off by default, for a system that
  reads health itself). Its store health is `dagda.spaces`.
- **Louhi:** confirmed. Its checks push on its 10 s status loop with valid_for of three
  intervals: louhi.runtime, louhi.memory_limits, louhi.user_namespaces, louhi.storage_bounds,
  louhi.cpu_limits, louhi.pids_limits, louhi.store. Asks: `checks()` stable and **ordered by
  name**, with since_unix_ms and the reason, so its NodeStatus comes from health() and checks().
  It refuses RunTask while draining or not_ready.
- **Kvasir:** confirmed. Refuses new clients while not_ready; on draining, stops accepting,
  moves proposer stickiness, lets transactions finish to a deadline. Pushes `kvasir.ranges`.
- **Lugh:** confirmed, one correction: /ready is unauthenticated, so its 503 body **names the
  checks that are not ready and carries no reason text** (a reason can hold a figure or a path).
  Reasons stay on Status and telemetry, behind the caller's credential. Pushes `lugh.hashing`
  and `lugh.caught_up`.
- **Macha:** awaited.

## As built (2026-11-23)

- `dannan.gates` is dropped: the class gates pace work and never refuse it, so "refused for
  more than 5 s" has no honest reading. Memory pressure is `dannan.memory` (degraded while the
  ledger's headroom above its control reserve is used up). Dannan's checks are dannan.clock,
  dannan.memory, dannan.certificate (with a certificate), and dannan.drain for the drain.
- A starting node is not_ready but advertises it only after health_hold, so a startup shorter
  than the hold never moves placement; a long recovery does, as it should.
- rendezvous takes `ignore_health` as an argument (it has no Scheme).
- Context's threads() gained a const overload beside readiness()'s (Macha's audit asked both).
