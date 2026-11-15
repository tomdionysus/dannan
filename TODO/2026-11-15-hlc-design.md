# A hybrid logical clock in Dannan (0.9.0)

the maintainer, 2026-11-15: a hybrid logical clock (HLC) goes in Dannan if another stack project
would adopt it; Mimir needs it (its ruling 8: Mimir waits for it and writes no copy), and
Dagda will adopt it for the register map (slice 3). Lir would, if the maintainer reopens its
2026-11-04 retained-ordering decision. For review by Mimir, Dagda and Lir before it is
built.

## What it is

Kulkarni and Demirbas's HLC: a timestamp `(physical, logical)` that never goes backwards,
stays within a bound of wall time, and orders causally related events. Ties between nodes
break by node id, so a stamp with its node is a total order.

```cpp
namespace dannan::contract {
// 48 bits of wall milliseconds and 16 of counter in one u64, so a stamp compares as
// an integer and encodes in 8 bytes, big-endian on the wire.
struct HlcStamp {
  uint64_t value = 0;
  uint64_t unix_ms() const noexcept { return value >> 16; }
  uint16_t logical() const noexcept { return static_cast<uint16_t>(value); }
  auto operator<=>(const HlcStamp&) const = default;
};
}  // namespace dannan::contract

// On the Context: one clock per node, shared by every hosted system (Dagda's first
// condition).
class Clock {
 public:
  virtual ~Clock() = default;
  // A stamp for an event here: after every stamp this node has made or received.
  virtual contract::HlcStamp now() = 0;
  // A stamp received from a peer, merged; the stamp for its receipt. A stamp further
  // ahead of local wall time than the bound is refused (std::nullopt) and counted, never
  // merged (Dagda's third condition).
  virtual std::optional<contract::HlcStamp> receive(contract::HlcStamp remote) = 0;
  // How far ahead of local wall time a received stamp may be.
  virtual std::chrono::milliseconds max_ahead() const = 0;
};
```

## Rules

- `now()`: `physical = max(previous.physical, wall)`; logical is `previous.logical + 1`
  when the physical part did not move, else 0.
- `receive(r)`: refused if `r.physical > wall + max_ahead`. Otherwise
  `physical = max(previous.physical, r.physical, wall)`, and logical follows the paper's
  three cases.
- The logical part overflowing (65,536 events in one millisecond without the wall
  moving) moves the physical part on by one millisecond, never wraps.
- Wall time is the node's injected `WallClock`, so tests step it.

## Never backwards across a restart (Dagda's second condition)

The node persists a high-water mark of the physical part beside its sealed counters
(`identity/counters`, label `dannan/hlc`), noted every second by the roster loop and at
stop, as `note_counter` already is. At start the clock begins at
`max(wall, high_water + noted_interval)`, so a restart never reissues a stamp, even after
a wall clock set back. The cost: after a crash, stamps may jump up to the noted interval
ahead of the wall, within `max_ahead`.

## Status

Counted on Status: received stamps refused as too far ahead, the largest offset seen
ahead and behind, logical overflows. Carrying stamps on Dannan's own wire (heartbeats and
calls) would give cross-node causality and a skew measure; that is a separate 0.9.0 wire
question, not in this design.

## Tests, first

- Monotonic under a wall clock that stands still, steps back, and jumps ahead.
- `receive` of a stamp ahead (within the bound): later stamps are after it; beyond the
  bound: refused, counted, not merged.
- Logical overflow moves the physical part, never wraps.
- A restart after the wall was set back still issues stamps after every one before it.
- Two nodes exchanging stamps: causally ordered events stay ordered.

## Open for review

1. 48 and 16 bits: enough? 48 bits of milliseconds lasts to the year 10889; 16 bits is
   65,536 events per millisecond per node before the physical part is pushed.
2. The default `max_ahead`: 1 minute (proposed); a NodeConfig setting.
3. Lir: is the clock's stamp with its node id enough for retained ordering, or does Lir
   need the node's durability epoch in the tie-break too?
