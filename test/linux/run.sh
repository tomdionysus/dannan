#!/bin/sh
#
# Dannan - The Distributed Substrate
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# One build of the tree at /src into /out/<name> and its gate, by g++: "plain", or the
# sanitizer named. Its tail goes to /out/<name>.txt. "package" instead installs the
# library and builds and runs a consumer of the installed package (test/package).
set -eu
name="$1"
if [ "$name" = package ]; then
  version=$(sed -n 's/^project(dannan VERSION \([0-9.]*\).*/\1/p' /src/CMakeLists.txt)
  rm -rf /out/package
  cmake -S /src -B /out/package/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/out/package/prefix
  cmake --build /out/package/build
  cmake --install /out/package/build
  cmake -S /src/test/package -B /out/package/consumer -G Ninja -DCMAKE_PREFIX_PATH=/out/package/prefix -DDANNAN_EXPECTED_VERSION="$version" \
    > /out/package.txt 2>&1 && status=0 || status=$?
  if [ "$status" = 0 ]; then
    { cmake --build /out/package/consumer && LD_LIBRARY_PATH=/out/package/prefix/lib:/out/package/prefix/lib64 \
        /out/package/consumer/consumer "$version"; } >> /out/package.txt 2>&1 && status=0 || status=$?
  fi
  tail -8 /out/package.txt
  echo "package exit $status"
  exit "$status"
fi
case "$name" in
  plain) flags="" ;;
  asan) flags="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined" ;;
  tsan) flags="-fsanitize=thread -fno-omit-frame-pointer" ;;
  *) echo "unknown build $name"; exit 2 ;;
esac
cmake -S /src -B "/out/$name" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDANNAN_BUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$flags" -DCMAKE_SHARED_LINKER_FLAGS="$flags"
cmake --build "/out/$name" --target dannan_tests
cd "/out/$name"
export ASAN_OPTIONS=detect_stack_use_after_return=1 UBSAN_OPTIONS=print_stacktrace=1 TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 suppressions=/src/dannan/test/tsan-suppressions.txt"
./dannan_tests > "/out/$name.txt" 2>&1 && status=0 || status=$?
tail -5 "/out/$name.txt"
echo "$name exit $status"
exit "$status"
