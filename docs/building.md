# Building Dannan

## From source

Dannan needs a C++20 compiler, CMake 3.24 or later, Boost 1.83 or later (Asio,
headers only), OpenSSL 3 and yaml-cpp; GoogleTest for the tests.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TESTING=ON
cmake --build build -j8
./build/dannan_tests
```

| Option | Default | What it does |
|---|---|---|
| `DANNAN_BUILD_TESTING` | OFF | Builds `dannan_tests` (GoogleTest). |
| `DANNAN_BUILD_TOOLS` | ON | Builds the tools: `dannan-node`, `dannan-ca`, `dannan-custody`, `dannan-bench`, `dannan-import-macha`, `dannan-version`. Off for a project that fetches Dannan for the library alone. |
| `DANNAN_BUILD_FUZZING` | OFF | Builds each fuzz target as its own binary. |
| `DANNAN_WARNINGS_AS_ERRORS` | OFF | Treats warnings as errors. |
| `DANNAN_SANITIZE` | empty | `address` (ASan with UBSan) or `thread` (TSan): instruments every target Dannan builds. See below. |

## Sanitizers: `DANNAN_SANITIZE`

A sanitizer build of anything in the stack builds every stack library it links from
source, instrumented the same way. A gate that links an uninstrumented Dannan checks
only the layer above it: it misses races and memory errors inside Dannan and across
the boundary, and TSan can report falsely at that boundary. Never link the Debian
package, or a plain build, into a sanitizer build.

`DANNAN_SANITIZE` is the one switch:

- `address`: `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`
- `thread`: `-fsanitize=thread -fno-omit-frame-pointer`

It applies to every target Dannan builds (the library, `dannan::testing`,
`dannan::sim_checkers`, the tools and the tests) and to nothing of the consumer's own.
`dannan::dannan` carries the flags to whatever links it in the build tree, so the
consumer's executable links the sanitizer's runtime. An installed package never
carries them.

A project that fetches Dannan sets it before the fetch, from its own option:

```cmake
set(DANNAN_SANITIZE address)   # or thread; a plain variable wins over the cache entry (CMP0126)
FetchContent_Declare(dannan GIT_REPOSITORY https://github.com/tomdionysus/dannan.git GIT_TAG 0.11.0)
FetchContent_MakeAvailable(dannan)
```

or on its command line, `-DDANNAN_SANITIZE=thread`. A project's own targets take its
own flags as before. Dagda honours the same option for its targets and passes it to
the Dannan it fetches.

To check that a build is instrumented, look for the runtime's symbols among the
library's undefined ones:

```
nm -D --undefined-only build/dannan/libdannan.so.0.11.0 | grep -c __asan_    # or __tsan_
```

`tools/ci.sh asan` and `tools/ci.sh tsan` build through `DANNAN_SANITIZE` and make
this check before they run the suite.

Every TSan run, Dannan's or a dependant's, passes the stack's one list of suppressions
(`dannan/test/tsan-suppressions.txt`: Asio's internals, which any TSan build on Asio can
hit). `libdannan-dev` installs it, and CMake names its path as `DANNAN_TSAN_SUPPRESSIONS`,
whether Dannan was found installed or fetched:

```
TSAN_OPTIONS="suppressions=${DANNAN_TSAN_SUPPRESSIONS}" ./my_tests
```

A project adds a suppression here, not a list of its own.
