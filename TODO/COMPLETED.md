# Dannan - Completed Work

What has landed, newest first, with what shipped and the gate counts it
left green. `ACTIVE.md` is the plan and this is the record.

## Releases

None. Dannan has no code yet.

## 2026-11-03, evening -- step 0: the repository and its skeleton

- First commit `2a923f5` on `main` (the plan and project files);
  `develop` branched. `CLAUDE.md` is not committed: Tom's global
  gitignore excludes it.
- The skeleton: `project(dannan VERSION 0.1.0)`; shared libraries
  `dannan` and `dagda` (Dagda links Dannan, never the reverse); GoogleTest
  binaries `dannan_tests` and `dagda_tests` behind `DANNAN_BUILD_TESTING`;
  `DANNAN_WARNINGS_AS_ERRORS`; clang `-Wthread-safety`; the design's
  directory layout; `dannan-version --version` printing the repository
  version from the generated `dannan/version.h`; `LICENSE` (GPLv3) and
  `docs/principles-and-laws.md` (verbatim from Macha `a547ddf`, one
  provenance line). Fixtures: the gate and the free-port allocator.
- Every dependency from MacPorts: Boost 1.88 (`/opt/local/libexec/boost/1.88`),
  OpenSSL 3.6.5, GoogleTest 1.18.0 (Tom installed it the same evening;
  Homebrew's copy unlinked). Configure:

  ```
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TESTING=ON \
    -DDANNAN_WARNINGS_AS_ERRORS=ON -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local"
  ```
- **Gate: `dannan_tests` 7, `dagda_tests` 1, all passed**, Release,
  warnings as errors, AppleClang.

## 2026-11-03, evening -- the plan reversed: here first

- Tom: "I misplanned. It should be here first." The in-tree decision of
  2026-11-02 is superseded: Dannan and Dagda are written in this
  repository from the design, as their own libraries in their own style,
  and Macha is ported onto them piece by piece, linking a tag at each
  port step. `ACTIVE.md` is rewritten around twelve steps that alternate
  building here and porting in Macha, with the transport first (step 2)
  and Macha on it (step 3) before the rest of Dannan exists. Tom ran
  `git init` here; the first commit waits on his word.
- The morning's leftovers in `../macha` (the goldens branch
  `feature/dannan-golden-fixtures` at `6a797af`, gate 683 + 20 + 21 green;
  the unfinished two-process script; the 0.91.2 worktree and its runs)
  were deleted by the Macha session on Tom's instruction later the same
  evening. Macha's `develop` is `a547ddf` (0.91.3 `d61eb02`, 0.91.4
  `353e600`, both untagged), clean, `develop` and `main` its only
  branches. The goldens' commits stay in the reflog for about ninety days.
- Work done in Macha this morning that stands whichever way Dannan is
  built, because it is the port gate: the 21 transport proofs
  (`c71370e`, merged), the two goldens (on the branch), and the findings
  they produced (the dropped replies, fixed in 0.91.3; the example
  refused by its own loader, being fixed; the macOS flake).
- Recorded for the next session: scratch files this session wrote under
  `/private/tmp` (the harness scratchpad) are outside the filesystem
  boundary of `CLAUDE.md`; later scratch went under
  `../macha/build-macports/dannan-scratch/`. Scratch for Dannan goes
  under `build/` here from now on.

## 2026-11-03 -- the transport proofs written

- Tom: write every owed transport case before seam 0. On Macha's
  `feature/dannan-transport-proofs` (off `develop` at `6c7ec64`,
  uncommitted), `tests/test_rpc_cluster.cpp` gained 21 cases from the
  specification (docs/cluster.md, docs/configuration.md Network, the
  net.hpp contracts) and `FaultyLinks::dialled_sockets`; nothing outside
  `tests/` changed. Each passed alone and in three runs of the whole
  `rpc_cluster` group (114 of 114). Only the added lines were formatted;
  the file is not clang-format clean at the baseline (647 lines differ).
- Gate on the branch, laptop, Release, warnings as errors: **682 + 19 +
  21**, one failure, `availability/test_a_disk_coming_back_is_surveyed_without_a_namespace_change`,
  an existing flake (1 run in 5 on the untouched baseline source).
- Three cases failed first, all my mistakes, and were corrected:
  `write_key` writes one key per case, so "another cluster's key" needed
  its own content; a reused gate let a stuck call through; a handler
  answered DATA-lane pings with a reply that lane forbids.
- Found: **replies beyond 256 on one connection are dropped
  unanswered** (inventory, "Findings"), proven by thread dumps and counts,
  not fixed. Two behaviours need a test seam. Both are with Tom (ACTIVE,
  Waiting on Tom, 4).

## 2026-11-03 -- the transport proof column

- The inventory's "Transport and lanes" table has its proof column
  filled from a read of every candidate case body at `6c7ec64`: 7 rows
  proven, 12 partial, 1 with none, and 23 cases owed, each named in its
  row. Found on the way:
  - Read-ahead and foreground promotion (`promote_read_ahead`,
    `promote_foreground`, `RpcServer::promote_queued`) has no test; the
    inventory's citation of `test_media_playback.cpp` was wrong.
  - `invariants/test_rpc_pre_auth_admission_is_bounded` is Linux-only
    (it counts `/proc/self/task`), so the laptop gate never runs it; and
    `test_peer_sockets_use_delay_based_congestion_control` asserts
    nothing on macOS beyond `peer_socket_options(-1)` being false.
  - `TransportTraffic` is touched by no test.
  - Nothing has two peers configured with different frame limits, or a
    hello from a newer protocol.
- basemind set up in this folder (`basemind.toml`, the rules block in
  `CLAUDE.local.md`; gitignore it when this becomes a repository); Macha's
  index was current.

## 2026-11-03 -- the laptop on MacPorts, and the laptop baseline counts

- The laptop is on MacPorts for everything Macha links (Waiting on Tom,
  2, done). Installed: `boost188` 1.88.0, `libtorrent-rasterbar` 2.0.15
  (built against boost188), `ffmpeg7` 7.1.5, `yaml-cpp` 0.8.0,
  `miniupnpc` 2.3.3, `lua54` 5.4.8; `nghttp2` 1.69.0 was already there.
  Removed: Homebrew's `libtorrent-rasterbar`, `ffmpeg@7`, `miniupnpc`,
  `yaml-cpp`, `lua@5.4` (and the leaves only they used); MacPorts'
  `boost` meta-port and `boost176` (it put 1.76 in `/opt/local/include`);
  a hand-built Boost 1.91 in `/usr/local/include` and `/usr/local/lib`,
  which `macha/build` had resolved beside Homebrew's libtorrent: the
  mixed Boost behind the macOS torrent segfaults. Homebrew's `boost`
  (1.92) and `libnghttp2` are unlinked, not removed, because folly,
  watchman and Homebrew's curl use them.
- The configure line on the laptop, in a fresh `build-macports` (the old
  `build` cache names paths that are gone):

  ```
  PKG_CONFIG_PATH=/opt/local/libexec/ffmpeg7/lib/pkgconfig \
  cmake -S . -B build-macports -DCMAKE_BUILD_TYPE=Release \
    -DMACHA_WARNINGS_AS_ERRORS=ON \
    -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local/libexec/ffmpeg7;/opt/local" \
    -DPKG_CONFIG_EXECUTABLE=/opt/local/bin/pkg-config
  ```

  Every dependency resolves under `/opt/local` and every built binary
  loads only `/opt/local`, system and own libraries, except FUSE
  (`/usr/local/lib/libfuse3`, a system install, not a port). The
  "Using Homebrew libtorrent/miniupnpc" configure messages are false
  positives: `brew --prefix` exits 0 for a formula that is not installed
  (`macha/CMakeLists.txt:183-205`); the prefixes do not exist.
- **Laptop counts at `0.91.2` (`6c7ec64`), Release, warnings as errors:
  661 + 19 + 21, all passed**, the torrent suite included, so it counts.
  `run-tests.sh` is mode 100644 in Macha's tree; run it as
  `sh ./run-tests.sh build-macports`. The fi-1 counts at 0.91.2 are still
  to be recorded, a deploy-day act.
- AthenaSIP's `build` and `build-tests` caches name Homebrew's Boost 1.92
  and its removed yaml-cpp; they need a fresh configure against MacPorts
  (boost188 meets its 1.87 floor) before the next AthenaSIP build.

## 2026-11-03 -- the baseline and the decisions

- Tom landed the 0.91.1 and 0.91.2 work in Macha and tagged `0.91.2` at
  `6c7ec64` on `develop`; the tree is clean. The seams measure against it.
  Suite counts at that tag: not yet recorded.
- Tom answered the eight design questions (`ACTIVE.md`, Decisions):
  extraction proceeds on the laptop now; AthenaSIP's mechanics are
  Dannan's style, applied at the split in one commit with the rename; the
  store is Dagda; Boost.Asio from the start, as seam 0 before everything;
  head spaces carry `replicas: N` from the start; scope is looked up in
  the Lugh head with a credential fallback; TCP simultaneous open first
  and UDP later; two protocol bumps, 24 for level 0 and 25 for identity;
  the compose cluster only until level 0 is ready to try for real.
- The plan was updated to match: seam 0 added to the extraction proposal
  with its gate against the 0.91.2 binary; seam 8's replica policy; the
  bumps through the encoding policy; `<store>` replaced by Dagda.
- Three more answered later the same day: the laptop goes to MacPorts for
  everything; moved tests ride the Macha runner through the split and the
  split converts them to GoogleTest; Status is a Dannan noun, one surface
  per node. `ACTIVE.md` gained "Resume here" for the next session.
- This repository gained `CLAUDE.md`, `CONTRIBUTING.md` and
  `.clang-format` (AthenaSIP's), and Macha's `TODO/ACTIVE.md` a "Dannan
  extraction" section pointing here, above its section 1.

## 2026-11-03 -- the plan

- `TODO/` written: `ACTIVE.md` (the plan, the rules, the decisions, what
  waits on Tom), `2026-11-03-macha-extraction.md` (the seams against the
  tree at 0.91.0 plus the uncommitted 0.91.2 work, the file table, the
  seam gate, what AthenaSIP needs), `2026-11-03-macha-function-inventory.md`
  (every behaviour of the moved code with its destination and proof), and
  renderings of Tom's two design documents of 2026-11-02.
- Found on the way, and recorded as decisions in `ACTIVE.md`: the route
  table is full (128 slots, 127 in use), so the open route space must
  widen it first; two seams the designs assume but do not name (the config
  split, the `NodeRuntime` split) must precede the CMake boundary; the
  Dannan plan's "HTTP is the hosted system's" takes the extraction plan's
  HTTP seam off the critical path.
- Baseline counts not yet recorded (Next, 1): the last recorded Macha gate
  is 659 + 19 + 21 on fi-1 at 0.90.96 (2026-10-14).
