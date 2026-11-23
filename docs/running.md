# Running a node binary

What bounds the memory of any node binary in the stack (dannan-node, macha, dagda's tools,
lir-node, mimir-node, kvasir-node and the rest).

## Memory budgets

Dannan's `sched::RetainedMemoryLedger` bounds what Dannan itself holds:
- queued requests;
- replies waiting for the wire (128 MiB and 256 messages per session);
- reassembly (256 MiB per session).

Each is charged and refused past the budget. A hosted system's handlers hold their own memory
outside it: at most one reply per running worker for a handler that answers by returning, and
whatever a deferring handler keeps while it waits. A hosted system that keeps large buffers alive
across calls charges them to the same budget through `Context::budget()`:
- `reserve(work, owner, bytes)` takes room now, or nothing if there is none;
- `acquire(..., deadline)` waits for room;
- a buffer is charged for as long as its lease lives.

The budget then bounds how many large buffers are alive at once.
