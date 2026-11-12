#!/bin/sh
#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One CI step on Debian 13, as the nodes have it, in GitHub's runner or a local
# container; .github/workflows/ci.yml runs each as its own job.
#
#   tools/ci.sh check      the header block on every source, and clang-format
#   tools/ci.sh release    g++ release build and the tests
#   tools/ci.sh clang      clang++, warnings as errors with -Wthread-safety
#   tools/ci.sh asan       AddressSanitizer and UndefinedBehaviorSanitizer
#   tools/ci.sh tsan       ThreadSanitizer, with dannan/test/tsan-suppressions.txt
#   tools/ci.sh package    install, then build and run a consumer (test/package)
#   tools/ci.sh coverage   a coverage build's tests; lcov report build-ci/coverage.info
#
# Builds go under build-ci/ (DANNAN_CI_OUT to move it). Needs g++ cmake
# ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev libgmock-dev,
# and for its step clang, clang-format or lcov.

set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
out=${DANNAN_CI_OUT:-$top/build-ci}
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
step=${1:-}

# Warnings are errors under clang, as on the laptop. g++ warns of shadowed
# names, pedantic points and a libstdc++ false positive that the tree has not
# yet been cleared of (TODO/ACTIVE.md), so its builds report them and go on.
werror=OFF
configure() {
  dir="$1"
  shift
  cmake -S "$top" -B "$out/$dir" -G Ninja -DDANNAN_BUILD_TESTING=ON -DDANNAN_WARNINGS_AS_ERRORS="$werror" "$@"
  cmake --build "$out/$dir" -j "$jobs" --target dannan_tests
}

case "$step" in
  check)
    cd "$top"
    missing=0
    for f in $(git ls-files '*.h' '*.cpp' '*.h.in'); do
      if ! head -3 "$f" | grep -qx '// Dannan - The Distributed Substrate'; then
        echo "no header block: $f"
        missing=1
      fi
    done
    [ "$missing" = 0 ]
    git ls-files '*.h' '*.cpp' | xargs clang-format --dry-run -Werror
    ;;
  release)
    configure release -DCMAKE_BUILD_TYPE=Release
    "$out/release/dannan_tests"
    ;;
  clang)
    werror=ON
    CXX=clang++ configure clang -DCMAKE_BUILD_TYPE=Release
    "$out/clang/dannan_tests"
    ;;
  asan)
    flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
    configure asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    ASAN_OPTIONS=detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1 "$out/asan/dannan_tests"
    ;;
  tsan)
    flags="-fsanitize=thread -fno-omit-frame-pointer"
    configure tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
    TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$top/dannan/test/tsan-suppressions.txt" "$out/tsan/dannan_tests"
    ;;
  package)
    version=$(sed -n 's/^project(dannan VERSION \([0-9.]*\).*/\1/p' "$top/CMakeLists.txt")
    rm -rf "$out/package"
    cmake -S "$top" -B "$out/package/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out/package/prefix"
    cmake --build "$out/package/build" -j "$jobs"
    cmake --install "$out/package/build"
    cmake -S "$top/test/package" -B "$out/package/consumer" -G Ninja -DCMAKE_PREFIX_PATH="$out/package/prefix" -DDANNAN_EXPECTED_VERSION="$version"
    cmake --build "$out/package/consumer"
    LD_LIBRARY_PATH="$out/package/prefix/lib:$out/package/prefix/lib64" "$out/package/consumer/consumer" "$version"
    ;;
  coverage)
    configure coverage -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage -DCMAKE_SHARED_LINKER_FLAGS=--coverage
    find "$out/coverage" -name '*.gcda' -delete
    (cd "$out/coverage" && ./dannan_tests)
    ignore=inconsistent,inconsistent,mismatch,negative,unused
    lcov --quiet --capture --directory "$out/coverage" --ignore-errors "$ignore" --output-file "$out/all.info"
    lcov --quiet --extract "$out/all.info" "$top/dannan/*" --ignore-errors "$ignore" --output-file "$out/library.info"
    lcov --quiet --remove "$out/library.info" "$top/dannan/test/*" "$top/dannan/fuzz/*" --ignore-errors "$ignore" --output-file "$out/coverage.info"
    rm "$out/all.info" "$out/library.info"
    lcov --summary "$out/coverage.info" --ignore-errors "$ignore"
    ;;
  *)
    echo "usage: tools/ci.sh check|release|clang|asan|tsan|package|coverage" >&2
    exit 2
    ;;
esac
