# The clocks

A node has three clocks, each for one job.

## Steady time: deciding

dannan::contract::TimeSource is the time a component decides by: whether a deadline has passed, when
a lease is due, when a cached answer expires. It never goes backwards. It is **injected**: a
component is handed one at construction, and a test hands in a stepped source
(`test::SteppedTime`) and moves it, so a test of a thirty-second deadline takes no time and gives the
same answer every run.

A stepped source says when it moves (TimeSource::watch). A wait on it
(dannan::contract::TimeWait) then arms no timer while the clock stands still, so thousands of waits
on a frozen test clock cost nothing.

## Wall time: recording

dannan::contract::WallClock is the time a component records: when a node was last heard of, a
row's timestamp. It can be set back, so it never decides a deadline. It is injected too
(`test::SteppedWall`), in milliseconds and, from 0.12.0, microseconds.

## The hybrid logical clock: ordering

dannan::contract::HlcClock (Context::hlc()) issues **stamps** for events: a pair of wall milliseconds
and a counter, packed into one number. A stamp is always after every stamp the node has made or
received, stays close to wall time, and is never repeated, not even across a restart. So a
last-writer-wins register, or a commit order, can trust it.

Two rules keep it honest:

- **Admit before applying.** A node that receives a stamp from a peer calls `admit` before it
  applies what the stamp carries. A stamp more than five seconds ahead of this node's wall time is
  refused, so one node with a runaway clock cannot drag every stamp in the cluster into the future.
- **A node out of bounds issues nothing.** Each heartbeat a node measures every peer's clock offset.
  If its own clock is more than two seconds off more than half of the agreed roster, it stops
  issuing stamps (`now()` throws), and its health reads not ready, until it is back. One or two nodes
  can never form that majority against themselves.

Why a persisted ceiling: a stamp is never issued past a ceiling written to disk, so a node that
restarts begins after every stamp it issued before, even if the wall was set back meanwhile. The
ceiling is written only while stamps are issued: an idle node writes nothing.

## Where to read on

- dannan::contract::TimeSource, dannan::contract::WallClock, dannan::contract::TimeWait.
- dannan::contract::HlcClock and dannan::contract::HlcStamp.
- dannan::Nodes::clock_offset and dannan::Nodes::clock_in_bounds.

## Testing on simulated nodes

A test runs whole nodes in one seeded world, on its time and network: the same seed replays the same run. examples/simulated_test.cpp:

\include simulated_test.cpp
