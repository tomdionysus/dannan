# Running a node binary

Rules for deploying any node binary in the stack (dannan-node, macha, dagda's tools, lir-node,
mimir-node, kvasir-node and the rest). Each binary's systemd unit carries them; the libraries never
set them for a host.

## The allocator

Every node binary sets glibc's mmap threshold in its unit:

```
[Service]
Environment=MALLOC_MMAP_THRESHOLD_=1048576
```

Why: a node moves large buffers, such as 4 to 16 MiB objects, extents and transfer frames. glibc's
mmap threshold is dynamic. It starts at 128 KiB, but each free of a mapped chunk raises it to that
chunk's size, up to 32 MiB on 64-bit. After the first few transfers, every large buffer comes from a
malloc arena instead of its own mapping. Non-main arenas are seldom trimmed, so the freed buffers
stay resident. A burst of slow reads then leaves gigabytes behind that never return to the kernel.

A live node showed this in November 2026: about 6 GB resident against a 768 MB retained-memory
budget, after a disk slowed under repair. Its large anonymous regions all sat on 64 MiB arena
boundaries, with no mapping of a buffer's own.

Setting the threshold turns the dynamic rise off. A buffer of 1 MiB or more is then always mapped
and goes back to the kernel when freed. The cost is one mmap and munmap per large buffer,
negligible beside the disk or network transfer it carries.

`MALLOC_ARENA_MAX` may also cap the arenas, which bounds what smaller allocations can retain, at
some lock contention among a node's threads. Set it only after measuring a node's RSS and latency
with and without it.

Library code never calls `mallopt()`: a process has one allocator, and its policy is the host's.

To check a node: run `pmap -x <pid>`, where regions on 64 MiB boundaries with high RSS are arenas.
Or, from gdb, `call malloc_stats()`.

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

The budget then bounds how many large buffers are alive at once. The allocator setting above is
what makes their memory go back to the kernel when they are freed.
