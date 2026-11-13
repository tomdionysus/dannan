# Packaging

Dannan is packaged for Debian trixie, on x86-64 and arm64, as:

- `libdannan<major.minor>` (for example `libdannan0.6`): the shared library. Its
  SONAME is major.minor, `libdannan.so.0.6`. A patch release keeps the ABI (see
  "The ABI" below), so a host built against 0.6.0 loads any later 0.6 patch. A
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
  `libdannan0.6 (>= <the patch it was built with>)` (`dh_makeshlibs -V`).
- An ABI change is a minor release: the next minor's first release has no base
  to compare with.
- The check sees layout and symbols: the types reached through exported
  functions and the public headers. A changed behaviour inside an inline or
  template function in a header is compiled into the host, which a relink picks
  up. abidiff cannot see that, so such a change goes in the changelog.

A consumer's packaging, for example Dagda's:

```
Build-Depends: libdannan-dev (>= 0.6.0), libdannan-dev (<< 0.7~)
Depends: ${shlibs:Depends}          # gives libdannan0.6 (>= 0.6.0)
```

and in CMake `find_package(dannan 0.6.0 CONFIG REQUIRED)`.

The promise starts at 0.6.0, under a SONAME no earlier release used. 0.5.0 to
0.5.5 were all `libdannan.so.0.5`, yet 0.5.4 changed their ABI: against 0.5.3 it
removed five functions and changed a `NodeConfig` member's type. 0.5.6 alone used
the whole version, `libdannan.so.0.5.6`. Reusing `libdannan.so.0.5` would have
let a host built on 0.5.3 load an incompatible library (Tom, 2026-11-13). A host
built against any 0.5 release is rebuilt once, against 0.6.0.

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
before `0.6.0`. Release tags sit on `main`, so a build on `develop`, where
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
