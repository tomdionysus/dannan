# Packaging

Dannan is packaged for Debian trixie, on x86-64 and arm64, as:

- `libdannan<major.minor>` (for example `libdannan0.5`): the shared library. Its
  SONAME is major.minor, `libdannan.so.0.5`. A patch release keeps the ABI (see
  "The ABI" below), so a host built against 0.5.7 loads any later 0.5 patch. A
  minor release is a new SONAME and a new package, installable beside the old.
- `libdannan-dev`: the headers, the `libdannan.so` link and the CMake package
  configuration. It depends on exactly its own release's library.
  `find_package(dannan <version> CONFIG)` accepts that version or any later
  patch of its minor.
- `dannan-tools`: `dannan-ca`, `dannan-import-macha` and `dannan-version`.
- Debug symbols for each, as `-dbgsym` packages.

The packages are built with debhelper from `debian/`, driving the CMake build;
the CMake install rules decide where every file goes, and `debian/rules` splits
them between the packages.

## The ABI

Tom, 2026-11-13: within a minor the ABI is kept, so a hosted system pins one
Dagda tag and takes the Dannan minor that Dagda was built on.

- `tools/ci.sh abi` builds the library at the minor's last release tag and at
  the tree, both with debug information. It compares the two with libabigail's
  `abidiff` over the installed public headers. CI fails on an incompatible
  change.
- An addition is a patch's to make. A consumer's package therefore depends on
  `libdannan0.5 (>= <the patch it was built with>)` (`dh_makeshlibs -V`).
- An ABI change is a minor release: the next minor's first release has no base
  to compare with.

A consumer's packaging, for example Dagda's:

```
Build-Depends: libdannan-dev (>= 0.5.7), libdannan-dev (<< 0.6~)
Depends: ${shlibs:Depends}          # gives libdannan0.5 (>= 0.5.7)
```

and in CMake `find_package(dannan 0.5.7 CONFIG REQUIRED)`.

0.5.6 alone named its SONAME for the whole version (`libdannan.so.0.5.6`, in
package `libdannan0.5.6`). 0.5.7 returns to major.minor with the promise, so a
host built against 0.5.6 is rebuilt once, against 0.5.7.

## Building

On Debian trixie, natively for the machine's architecture, from a git
checkout:

```sh
sudo apt install dpkg-dev
sudo apt build-dep ./
tools/build-deb.sh
```

The packages are written to `build-deb/`. The tests run inside the build;
`DEB_BUILD_OPTIONS=nocheck` skips them. CI builds the packages on x86-64 and
arm64 and fails on any lintian error or warning.

## Versions

The version comes from `git describe`. The tag `0.5.6` gives `0.5.6`; a build
three commits later gives `0.5.6+git3.gabc1234`, which sorts after `0.5.6` and
before `0.5.7`. Release tags sit on `main`, so a build on `develop`, where
none is reachable, gives the tree's version and the commit,
`0.5.6+git.gabc1234`. Uncommitted changes add `.dirty`. A build of a tag must match
`project(dannan VERSION ...)` in `CMakeLists.txt`, and the library's package
named in `debian/control` must be `libdannan` and its major.minor, or the script
refuses it. A release's version commit changes the first two, and the third
when it starts a minor. Only builds of a
tag are released.

## Releasing

The packages of a release go into the shared tomcully.com apt repository, in
its `incoming/dannan/<version>/`, and are added and signed with its
`publish.sh`; the repository's README says how it is checked and uploaded.
