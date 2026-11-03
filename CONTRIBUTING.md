# Contributing to Dannan

## Read first

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Dannan and
Dagda are written here from Tom's design, and Macha (`../macha`) is ported onto
them piece by piece; the plan says which step is next and what its gate is. The
one rule: Macha loses no function. Dannan is written from the design, never
transcribed from Macha. No byte on disk or on the wire changes before level 0.

The four laws and five disciplines (`docs/principles-and-laws.md`, a verbatim copy
of Macha's) govern every change here, cited by number. A change that touches
scheduling, admission, priority, retry or recovery says which law it serves and
which discipline it follows.

## Branches

- `main` is the last stable state. Nothing is committed to it directly; stable
  states reach it by merging `develop`.
- `develop` is where work happens.
- Work that needs isolation branches off `develop` as `feature/<short-name>` and
  merges back into `develop`.

## Tags and releases

- Tags are exactly `x.y.z` semver: annotated, no `v` prefix, no suffix, cut from
  `main`.
- The tag, the CMake `project(... VERSION ...)` and `--version` are the same
  number.

## Commits

- Subjects are short imperative prose. No emoji, no ticket prefixes.
- Keep unrelated changes in separate commits: a move, a rename, a fix and a feature
  are four commits. A commit that moves files does not change their content.
- A commit message ends with its last line of prose. No tool attribution trailers.

## Building and testing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TESTING=ON \
  -DDANNAN_WARNINGS_AS_ERRORS=ON -DCMAKE_PREFIX_PATH="/opt/local/libexec/boost/1.88;/opt/local"
cmake --build build -j8
./build/dannan_tests && ./build/dagda_tests
```

Run the test binaries rather than `ctest`. Everything runs on a developer machine
(MacPorts for every dependency on the laptop); there is no hosted CI. A port step
builds and tests in `../macha` as well (`CONTRIBUTING.md` and `tests/TESTING.md`
there; the configure line for the laptop is in `TODO/COMPLETED.md`).

Write tests from the design, the laws and the function inventory, not from the
current behaviour.

## Dependencies

Boost (Asio) and OpenSSL, plus GoogleTest for the test build. Prefer writing
something by hand over adding a library, and say so if a dependency really is
the right call.

## Style

- Formatting is `.clang-format` (Google base, 160 columns): run `clang-format -i`
  on the files you changed. Two-space indent, `#pragma once`, braces on the same
  line, headers `.h`.
- `snake_case` for functions, methods, variables and files; `PascalCase` for
  types; private and protected members prefixed with `_`. Namespaces are
  `dannan::<subsystem>` and `dagda::<subsystem>`.
- Every source file starts with the Dannan header block (`CLAUDE.md` has it).
- No emoji anywhere.
- Dependency injection over globals; the only process global is the io context.
- Each component logs through its own scoped logger.
- Thread-safety annotations on every guarded member; contracts declare their waits
  and thread safety beside each operation.
- Comments are sparse and explain intent, not mechanics.

## Plan

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Move items
across as they land.
