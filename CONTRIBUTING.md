# Contributing to Dannan

## Read first

`TODO/ACTIVE.md` is the plan and `TODO/COMPLETED.md` is the record. Dannan is
the network layer of a stack of three projects with one set of conventions:
Dannan (`../dannan`), Dagda the store (`../dagda`) and Lugh the directory
(`../lugh`). `../dagda/TODO/2026-11-03-stack.md` says what each owns. Dagda and
Lugh use Dannan as their network layer, Lugh uses Dagda as its datastore, and
no code is duplicated between the three (Tom, 2026-11-03). Macha
(`../macha`) is the first customer: the design is complete when Macha can be
refactored onto it. Dannan is written from the design, never transcribed from
Macha, and has its own wire, versioned from 1.

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
(MacPorts for every dependency on the laptop); there is no hosted CI.
Sanitizers run before a release, or to hunt a bug that cannot be pinned any
other way. The compose bed (`python3 test/compose/scenario.py`, Docker
running) proves level 0 through real NATs; it is run by hand, never in the
per-commit gate.

Write tests from the design, the laws and the function inventory, not from the
current behaviour.

## Dependencies

Boost (Asio) and OpenSSL, plus GoogleTest for the test build. Prefer writing
something by hand over adding a library, and say so if a dependency really is
the right call.

## Layout

One layout for all three projects (Tom, 2026-11-03: "identical styles,
conventions, and layouts"). For a project `<p>` (`dannan`, `dagda`, `lugh`):

```
CMakeLists.txt               the project, options <P>_BUILD_TESTING,
                             <P>_BUILD_FUZZING, <P>_WARNINGS_AS_ERRORS;
                             no presets
<p>/CMakeLists.txt           the library target <p>
<p>/include/<p>/             the public headers a consumer includes as
                             "<p>/x.h"
<p>/<subsystem>/x.h, x.cpp   everything else, header and source side by side,
                             included as "<p>/<subsystem>/x.h"
<p>/contract/                interfaces with declared waits and thread safety
<p>/test/                    GoogleTest: <subsystem>_<thing>_test.cpp, flat,
                             one binary <p>_tests; fixtures in
                             <p>/test/fixtures/ (header-only); anything
                             loaded at test time (plugins) in its own
                             directory beside them
<p>/fuzz/                    libFuzzer targets, <format>_fuzz.cpp, one per
                             parser of untrusted bytes, built only with
                             <P>_BUILD_FUZZING and clang
tools/                       executables, <p>_<tool>.cpp building <p>-<tool>
test/compose/                multi-node beds (Docker), run by hand
docs/                        how it works
TODO/ACTIVE.md, COMPLETED.md the plan and the record
CLAUDE.md, CONTRIBUTING.md   the rules (CLAUDE.md is gitignored globally)
```

A file pair holds one component: a class and what serves only it. Names are
`snake_case` files, `PascalCase` types, `_member`s, namespaces
`<p>::<subsystem>`.

## Shared code

What more than one project needs lives once, in the lowest project that
needs it, and the others link it: Dannan's crypto primitives
(`identity/crypto.h`), durable files (`identity/durable_file.h`), the byte
codec (`net/codec.h`), the thread-safety contract and time sources
(`contract/`), logging (`status/log.h`, a logger from `Context::logger`) and
the Status surface. A project that finds itself writing something another has
asks that project's session to add it there instead.

## Errors

An operation that could not happen throws (`std::invalid_argument` for a
caller's mistake, `std::runtime_error` or a named subclass such as
`net::DecodeError` for bytes or a system that refused). An operation that
happened returns its result, and one that happened on more than one node
returns its evidence (who answered, what failed and why), never a bare
boolean. A read that finds nothing succeeds with an empty value.

## Style

- Formatting is `.clang-format` (Google base, 160 columns): run `clang-format -i`
  on the files you changed. Two-space indent, `#pragma once`, braces on the same
  line, headers `.h`.
- `snake_case` for functions, methods, variables and files; `PascalCase` for
  types; private and protected members prefixed with `_`. Namespaces are
  `<project>::<subsystem>`.
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
