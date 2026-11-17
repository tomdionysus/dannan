# A hybrid logical clock in Dannan (0.9.0)

the maintainer, 2026-11-15: a hybrid logical clock (HLC) goes in Dannan if another stack project
would adopt it; Mimir needs it (its ruling 8: Mimir waits for it and writes no copy), and
Dagda will adopt it for the register map (slice 3). Lir would, if the maintainer reopens its
2026-11-04 retained-ordering decision. Reviewed by Dagda, Lir and Mimir, 2026-11-15; their
points are taken below ("Review").

## What it is

Kulkarni and Demirbas's HLC: a timestamp `(physical, logical)` that never goes backwards,
stays within a bound of wall time, and orders causally related events. Ties between nodes
break by node id (never reused), so a stamp with its node is a total order. Lir and Mimir
need no durability epoch in the tie-break: the ceiling below means a node never issues the
same stamp twice, across restarts too.

```cpp
namespace dannan::contract {
// 48 bits of wall milliseconds and 16 of counter in one u64, so a stamp compares as
// an integer.
struct HlcStamp {
  uint64_t value = 0;
  uint64_t unix_ms() const noexcept { return value >> 16; }
  uint16_t logical() const noexcept { return static_cast<uint16_t>(value); }
  auto operator<=>(const HlcStamp&) const = default;
};
// The one encoding every system writes: 8 bytes, big-endian (Mimir's review, rule 3).
std::array<uint8_t, 8> encode(HlcStamp stamp) noexcept;
HlcStamp decode(std::span<const uint8_t, 8> bytes) noexcept;
}  // namespace dannan::contract

// What receive() did with a peer's stamp.
struct HlcReceipt {
  // This node's stamp for the receipt: after the received one when it was merged.
  contract::HlcStamp local;
  // False: the stamp was further ahead of local wall time than max_ahead, and the clock
  // was not pulled forward to it.
  bool merged = true;
  // How far the received stamp's physical part was from local wall time (positive:
  // ahead), for the caller to log (Lir's review).
  std::chrono::milliseconds offset{0};
};

// On the Context: one clock per node, shared by every hosted system (Dagda's first
// condition). Thread-safe; now() and receive() wait on nothing and hold no lock: one
// compare-and-swap on a u64 (Mimir's review: they are on its write path, from many
// strands). Persisting the ceiling is the only wait, off that path but for a jump
// past it (below).
class Clock {
 public:
  virtual ~Clock() = default;
  // A stamp for an event here: after every stamp this node has made or received.
  virtual contract::HlcStamp now() = 0;
  // A peer's stamp, merged into this clock unless it is too far ahead.
  virtual HlcReceipt receive(contract::HlcStamp remote) = 0;
  // How far ahead of local wall time a received stamp may be (NodeConfig, default one
  // minute).
  virtual std::chrono::milliseconds max_ahead() const = 0;
};
```

**A refusal is the clock's, not the data's** (Dagda's, Lir's and Mimir's reviews).
`merged = false` means only that this node's clock was not pulled forward. The event that
carried the stamp is the hosted system's: a replicated entry keeps its stamp and is merged
by it on every replica, or replicas diverge. Mimir and Dagda apply such an entry by its
stored stamp and count it; Lir orders a client's write by its own `now()` and counts it.
What to do about a node far ahead is the operator's.

## Rules

- `now()`: `physical = max(previous.physical, wall)`; logical is `previous.logical + 1`
  when the physical part did not move, else 0.
- `receive(r)`: not merged if `r.physical > wall + max_ahead`. Otherwise
  `physical = max(previous.physical, r.physical, wall)`, and logical follows the paper's
  three cases.
- The logical part overflowing (65,536 events in one millisecond without the wall
  moving) moves the physical part on by one millisecond, never wraps. A sustained storm
  can push the clock ahead of the wall without bound and its stamps then be refused by
  peers; overflows are counted on Status (Mimir's note).
- Wall time is the node's injected `WallClock`, so tests step it.

## Never backwards across a restart: a ceiling, not a note

Dagda's and Lir's reviews found that a note of the high-water mark every second can
reissue stamps: `receive()` may pull the clock up to `max_ahead` forward at once, and
overflow pushes it on, so a crash before the next note restarts the clock below stamps
already issued. So the mark is a reservation, as Lir reserves sequence blocks:

- The node persists a **ceiling** for the physical part beside its sealed counters
  (`identity/counters`, label `dannan/hlc`, durable before it is used).
- No stamp's physical part passes the persisted ceiling. The roster loop raises the
  ceiling to `issued + 2 s` every second, so in the steady state the write path never
  waits.
- A stamp that would pass the ceiling (a `receive()` far ahead, an overflow storm) raises
  it first, synchronously: one durable write on a rare path, the only wait `now()` or
  `receive()` can meet. Stated in the contract.
- At start the clock begins at `max(wall, ceiling)`, so no stamp before the crash is
  issued again, even after the wall clock was set back.

## Status

Counted on Status: stamps not merged (too far ahead), logical overflows, ceiling raises
off the roster loop. Per peer (Mimir's review: the skew figure an operator needs, since
TTLs are absolute wall times): the largest offset seen ahead and behind, from the stamps
`receive()` is given. Carrying stamps on Dannan's own wire (heartbeats and calls), which
would measure skew without a hosted system's traffic, is a separate 0.9.0 wire question.

## Tests, first

- Monotonic under a wall clock that stands still, steps back, and jumps ahead.
- `receive` within the bound: later stamps are after it; beyond it: not merged, counted,
  the offset given, the clock unmoved.
- Logical overflow moves the physical part, never wraps.
- A restart after the wall was set back still issues stamps after every one before it.
- A receive 30 s ahead, then a crash before the roster loop's next raise: after restart
  the first stamp is after the received one (Dagda's and Lir's test).
- `now()` from many threads at once: every stamp distinct and each thread's increasing.
- Encode and decode round-trip and order as the stamps do.
- Two nodes exchanging stamps: causally ordered events stay ordered.

## Review

- Dagda: the restart rule's defect (taken: the ceiling); refusal is the clock's (taken);
  48/16 and a one-minute bound fine.
- Lir: stamp and node id enough, no epoch; the ceiling as a reservation (taken); a
  refusal the caller's, with its offset (taken); expiry wrong by at most the skew that
  `max_ahead` caps. Adoption waits on the maintainer reopening the 2026-11-04 decision, which Lir
  raises when 0.9.0's design is settled.
- Mimir: refusal not the data's (taken); thread-safe, lock-free (taken); one encoding
  (taken); per-peer offsets on Status (taken); 48/16 and one minute fine.

## As built (2026-11-17, step 5)

`dannan/contract/hlc.h` and `.cpp`; on the node, `Context::hlc()`. Three choices made in
building it, to be confirmed:
- **The interface is `HlcClock`**, not `Clock`: `contract::Clock` is already the steady clock.
- **`receive(stamp, peer)`** beside `receive(stamp)`: per-peer offsets on Status need the peer,
  which the reviewed `receive` does not carry. It takes a short lock for the per-peer table;
  `receive(stamp)` and `now()` stay lock-free.
- **A node without key custody** has no sealed counters, which the design names as the
  ceiling's home: it keeps the ceiling in `<state>/clock/hlc-ceiling`, a durable file. That is
  as safe across a crash; it is not sealed, so a state directory restored alone is not refused.

At a restart the clock begins at the wall, or one past the ceiling if the wall is not past it,
so no stamp is reissued. Carrying stamps on Dannan's own wire stays a separate question.

