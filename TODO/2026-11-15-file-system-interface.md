# One file-system interface for the stack (0.9.0)

Asked by Dagda (2026-11-15), for the simulation harness (the maintainer: built in Dannan within 0.9.0)
so that real Dagda, Kvasir's and Mimir's stores on it included, runs above a simulated disk.
Under rule 3, beside `identity/durable_file.h`, which already lives in Dannan. For review by
Dagda before it is built; Kvasir and Mimir are told.

## Why

Dagda's state reaches the disk three ways (Dagda, 2026-11-15): through Dannan's
`durable_replace_file`; through its own POSIX code (the sealed journal's appends and
`fsync_checked`, the durability domain's `fsync` and `syncfs`); and through its
`LocalStoreFiles` seam, which has no sync. A simulated disk can sit under none of the
second, and the third syncs elsewhere. One interface for every path closes it: Dagda routes
paths 2 and 3 through it (Dagda's work, after its small-file and ack-level work), and slice
4's node-local log is written on it from the start.

## The interface

```cpp
namespace dannan::storage {

// A file system as the stack writes it. Every operation names its path and returns what
// happened; nothing throws for an I/O failure, so a fault is a value the caller decides on.
class FileSystem {
 public:
  virtual ~FileSystem() = default;
  virtual Result<File> open(const Path&, OpenMode) = 0;   // read, write, create, exclusive
  virtual Result<void> rename(const Path& from, const Path& to) = 0;   // atomic replace
  virtual Result<void> remove(const Path&) = 0;
  virtual Result<std::vector<std::string>> list(const Path& directory) = 0;
  virtual Result<void> make_directories(const Path&) = 0;
  // Makes a directory's entries (creates, renames, removes) durable.
  virtual Result<void> sync_directory(const Path&) = 0;
  // Makes everything written on the file system durable (syncfs).
  virtual Result<void> sync_all(const Path& any_on_it) = 0;
};

class File {
  Result<size_t> read_at(uint64_t offset, std::span<uint8_t>);
  Result<size_t> write_at(uint64_t offset, std::span<const uint8_t>);
  Result<size_t> append(std::span<const uint8_t>);
  Result<void> truncate(uint64_t size);
  Result<uint64_t> size();
  // Makes this file's data durable (fsync). See "The sync contract".
  Result<void> sync();
};

// The device: POSIX, as the stack writes today.
FileSystem& posix_file_system();
}
```

`durable_replace_file` and Dannan's other durable writes take a `FileSystem&` (defaulting
to the POSIX one), so they run on the simulated disk unchanged.

## The sync contract

What `sync()` (and `sync_directory`, `sync_all`) says, and what it cannot:
- **Success** means the device reported the data durable. On POSIX that is `fsync`'s word;
  the interface does not and cannot know whether the device lied.
- **A failed sync** is reported as an error, naming the path and the errno. After a failed
  sync, what the file holds on disk is unknown (Linux may drop the dirty pages and report
  later syncs as successful: the "fsyncgate" behaviour), so a caller must treat the file as
  suspect: Dagda latches its domain and heals from disk, which is the right response, and
  the contract says so.
- **A lying sync** (success reported, bytes lost) and **a torn write** (a prefix of a write
  durable after a crash) are not reportable by any device: they are what the simulation
  injects, so a caller's recovery is tested against them. The contract states that a
  caller must survive both: checksums or seals on what it reads back (Dagda's sealed
  journal and DNNCNT01 files already do), and no reliance on write atomicity beyond what
  `rename` gives.

## The simulated disk

`test::sim::Disk` implements `FileSystem` over the world: each file has a durable image and
pending writes; `sync` moves pending writes to durable, or, as the seed says, fails, lies,
or makes a prefix durable; a crash keeps only the durable images. Paths, renames and
directory entries follow the same rule (an unsynced rename may be lost).

## Dagda's review, taken

- **Result, with the errno's class.** An error carries the path, the errno, and its class,
  because callers act on each differently: `no_room` (ENOSPC, EDQUOT: a refusal, try the
  next backend), `absent` (ENOENT on open or read: not a failure), `io` (EIO, and every
  failed sync: Dagda latches its domain), `exists`, `denied`, `other`. Beside it one
  throwing helper, `or_throw()`, raising an exception that carries the same error, so
  code that throws today (`durable_replace_file`'s callers, Dagda's LocalStore) moves in
  one step.
- **list() returns entries with their type and size** (one stat each), and `stat(path)`
  beside it: Dagda's presence-index warm-up and pack scan list thousands of files at start,
  and a names-only list then a size per file would be a startup regression on a Pi.
  `LocalStoreFiles` maps without loss: install is open(create, exclusive) a temporary,
  write, rename, with no per-file sync (the domain's batch sync covers it).
- **sync_all (syncfs) stays.** The durability domain makes a whole window's writes
  durable with one syncfs per backend: the group commit behind the 500 ms window. In the
  simulation, everything pending on that file system becomes durable, or fails, or lies, as
  the seed says.
- **space(path)**: available and total, statvfs-style, for the pool's room checks; the
  simulation can fill a disk, which is how "no room" paths and Macha's full-node spill are
  tested.
- **lock(path)**: the advisory lock (flock), held for a handle's life, for the migration
  manifest and a backend root held against a second process; the simulation runs two
  processes' worth on one disk honestly. Dannan's own state lock moves onto it.
- Not needed: fallocate, O_DIRECT.
- Dagda's raw syncs in scope for its routing work, after its small-file and ack-level work:
  the sealed journal, the durability domain and storage pool, `heads/replica.cpp`,
  `heads/node_pack.cpp`, `store/local_store.cpp`, `migrate/migrate.cpp` and
  `migrate/manifest.cpp`.
