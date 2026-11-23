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
#   tools/ci.sh release    g++ release build, warnings as errors, and the tests
#   tools/ci.sh clang      the same with clang++ (and -Wthread-safety)
#   tools/ci.sh asan       AddressSanitizer and UndefinedBehaviorSanitizer
#   tools/ci.sh tsan       ThreadSanitizer, with dannan/test/tsan-suppressions.txt
#   tools/ci.sh package    install, then build and run a consumer (test/package)
#   tools/ci.sh coverage   a coverage build's tests; lcov report build-ci/coverage.info
#   tools/ci.sh abi        the library against the minor's last release, by abidiff
#   tools/ci.sh docs       the API reference and guides by Doxygen, warnings as errors: an
#                          undocumented public declaration in a gated header fails it
#   tools/ci.sh install-debs <tag>
#                          for a dependant's CI: builds the tag's libdannan<major.minor>
#                          and libdannan-dev with tools/build-deb.sh (tests skipped) and
#                          installs them; DANNAN_REPOSITORY names another clone source, and
#                          DEB_BUILD_OPTIONS (parallel=N, say) is kept, with nocheck added
#
# Builds go under build-ci/ (DANNAN_CI_OUT to move it). Needs g++ cmake
# ninja-build libboost-dev libssl-dev libyaml-cpp-dev libgtest-dev libgmock-dev,
# and for its step clang, clang-format, lcov, abigail-tools (abi; the tags fetched), or doxygen.

set -eu

top=$(cd "$(dirname "$0")/.." && pwd)
out=${DANNAN_CI_OUT:-$top/build-ci}
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
step=${1:-}

# Warnings are errors in every build but the sanitizers': instrumented, g++ warns
# inside libstdc++ (regex, -Wmaybe-uninitialized) and Asio (-Wtsan, its fences),
# not in Dannan. The release and clang steps hold the warning gate.
configure() {
  dir="$1"
  shift
  cmake -S "$top" -B "$out/$dir" -G Ninja -DDANNAN_BUILD_TESTING=ON -DDANNAN_WARNINGS_AS_ERRORS=ON "$@"
  cmake --build "$out/$dir" -j "$jobs" --target dannan_tests
}

# The library itself carries the sanitizer, not only the tests that link it: its undefined
# symbols name the runtime's entry points (`prefix`, __asan_ or __tsan_).
instrumented() {
  library=$(find "$1" -maxdepth 2 -name 'libdannan.so*' -type f | head -n 1)
  count=$(nm -D --undefined-only "$library" | grep -c "$2" || true)
  if [ "$count" -eq 0 ]; then
    echo "$library is not instrumented: no $2 symbols" >&2
    exit 1
  fi
  echo "$library: $count $2 symbols"
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
    CXX=clang++ configure clang -DCMAKE_BUILD_TYPE=Release
    "$out/clang/dannan_tests"
    ;;
  asan)
    # Through DANNAN_SANITIZE, the switch a consumer fetching Dannan uses, so the gate proves it.
    configure asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDANNAN_WARNINGS_AS_ERRORS=OFF -DDANNAN_SANITIZE=address
    instrumented "$out/asan" __asan_
    ASAN_OPTIONS=detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1 "$out/asan/dannan_tests"
    ;;
  tsan)
    configure tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDANNAN_WARNINGS_AS_ERRORS=OFF -DDANNAN_SANITIZE=thread
    instrumented "$out/tsan" __tsan_
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
    # The library alone, for a project that fetches Dannan for it (Lugh): no tool is
    # built, and tests without the tools they run are refused at configure.
    cmake -S "$top" -B "$out/package/library-only" -G Ninja -DCMAKE_BUILD_TYPE=Release -DDANNAN_BUILD_TOOLS=OFF
    cmake --build "$out/package/library-only" -j "$jobs"
    for tool in dannan-node dannan-ca dannan-custody dannan-bench dannan-import-macha dannan-version; do
      if [ -e "$out/package/library-only/$tool" ]; then
        echo "package: DANNAN_BUILD_TOOLS=OFF still built $tool" >&2
        exit 1
      fi
    done
    if cmake -S "$top" -B "$out/package/refused" -G Ninja -DDANNAN_BUILD_TESTING=ON -DDANNAN_BUILD_TOOLS=OFF > /dev/null 2>&1; then
      echo "package: DANNAN_BUILD_TESTING without DANNAN_BUILD_TOOLS was not refused" >&2
      exit 1
    fi
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
  abi)
    # A patch release keeps the ABI: the library is compared with the last release
    # of its minor, both built with debug information and judged by their public
    # headers. Any change abidiff reports fails it: a changed type sets only its
    # "change" bit (4), never "incompatible" (8), which it keeps for removals.
    # Added symbols are left out of the report, since an addition is a patch's to
    # make. A virtual added to an interface changes its vtable, and is reported.
    # The standard library's template instances are left out
    # (tools/abi-suppressions.ini): they come and go with Dannan's code, not its
    # interface.
    # The first release of a minor has nothing to keep.
    version=$(sed -n 's/^project(dannan VERSION \([0-9.]*\).*/\1/p' "$top/CMakeLists.txt")
    minor=${version%.*}
    # Between releases the tree may still say the last one's version, which is
    # then the base; a build of a release tag is compared with the one before.
    skip=
    if [ "$(git -C "$top" describe --exact-match --tags HEAD 2>/dev/null)" = "$version" ]; then
      skip=$version
    fi
    base=$(git -C "$top" tag --list "$minor.*" --sort=-v:refname | grep -vx "${skip:-none}" | head -n 1)
    # DANNAN_ABI_BASE names another release to compare with, such as one a live
    # host was built on.
    base=${DANNAN_ABI_BASE:-$base}
    if [ -z "$base" ]; then
      echo "abi: no earlier $minor release; $version may change the ABI"
      exit 0
    fi
    rm -rf "$out/abi"
    mkdir -p "$out/abi"
    git -C "$top" archive --prefix=base/ "$base" | tar -x -C "$out/abi"
    for side in base current; do
      src="$out/abi/base"
      [ "$side" = current ] && src="$top"
      cmake -S "$src" -B "$out/abi/$side-build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDANNAN_BUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$out/abi/$side"
      cmake --build "$out/abi/$side-build" -j "$jobs"
      cmake --install "$out/abi/$side-build"
    done
    old=$(find "$out/abi/base" -name 'libdannan.so.*.*.*' | head -n 1)
    new=$(find "$out/abi/current" -name 'libdannan.so.*.*.*' | head -n 1)
    # The releases before the promise named their SONAME for the whole version;
    # the one move from that to major.minor is the promise itself, not a break.
    ignore=
    if objdump -p "$old" | grep -q "SONAME *libdannan.so.$base\$"; then
      ignore=--ignore-soname
    fi
    echo "abi: $version against $base"
    set +e
    abidiff $ignore --no-added-syms --no-show-locs --suppressions "$top/tools/abi-suppressions.ini" --headers-dir1 "$out/abi/base/include/dannan" --headers-dir2 "$out/abi/current/include/dannan" "$old" "$new"
    rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
      echo "abi: $version breaks $base's ABI (abidiff $rc): a patch may not; make it a minor release" >&2
      exit 1
    fi
    ;;
  docs)
    rm -rf "$out/docs"
    cmake -S "$top" -B "$out/docs" -G Ninja -DDANNAN_BUILD_DOCS=ON -DDANNAN_BUILD_TOOLS=OFF
    cmake --build "$out/docs" --target docs
    ;;
  install-debs)
    # One step every dependant's CI shares (Lir, Lugh, Louhi, Mimir, Kvasir, Dagda), so none
    # builds Dannan's packages its own way. The tag's own packaging builds them, so a dependant
    # pinned to an older tag gets that tag's packages. Needs git, dpkg-dev, apt and root or sudo.
    tag=${2:?usage: tools/ci.sh install-debs <tag>}
    repository=${DANNAN_REPOSITORY:-https://github.com/tomdionysus/dannan.git}
    sudo=
    if [ "$(id -u)" -ne 0 ]; then sudo=sudo; fi
    work="$out/install-debs"
    rm -rf "$work"
    mkdir -p "$work"
    git clone --quiet --depth 1 --branch "$tag" "$repository" "$work/src"
    $sudo apt-get install -y --no-install-recommends dpkg-dev
    $sudo apt-get build-dep -y "$work/src"
    # The caller's options kept, nocheck added: a caller's parallel= holds the build to the cores it
    # claimed (Mimir's, a 1.5 GB container on one core).
    (cd "$work/src" && DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:+$DEB_BUILD_OPTIONS }nocheck" tools/build-deb.sh)
    $sudo apt-get install -y "$work"/src/build-deb/libdannan[0-9]*_*.deb "$work"/src/build-deb/libdannan-dev_*.deb
    dpkg-query -W -f='${Package} ${Version}\n' 'libdannan*'
    ;;
  *)
    echo "usage: tools/ci.sh check|release|clang|asan|tsan|package|coverage|abi|docs|install-debs <tag>" >&2
    exit 2
    ;;
esac
