# Packaging

Dannan is packaged for Debian trixie, on x86-64 and arm64, as:

- `libdannan<version>` (for example `libdannan0.5.6`): the shared library. Its
  SONAME is the whole version, `libdannan.so.0.5.6`, because no release
  promises another's ABI yet; each release is its own package, so two can be
  installed side by side and a host built against one refuses to load another.
- `libdannan-dev`: the headers, the `libdannan.so` link and the CMake package
  configuration. It depends on exactly its own release's library, and
  `find_package(dannan <version> CONFIG)` accepts only that exact version.
- `dannan-tools`: `dannan-ca`, `dannan-import-macha` and `dannan-version`.
- Debug symbols for each, as `-dbgsym` packages.

The packages are built with debhelper from `debian/`, driving the CMake build;
the CMake install rules decide where every file goes, and `debian/rules` splits
them between the packages.

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
before `0.5.7`; uncommitted changes add `.dirty`. A build of a tag must match
`project(dannan VERSION ...)` in `CMakeLists.txt`, and the library's package
named in `debian/control` must be `libdannan` and that version, or the script
refuses it. A release's version commit changes all three. Only builds of a
tag are released.

## Releasing

The packages of a release go into the shared tomcully.com apt repository, in
its `incoming/dannan/<version>/`, and are added and signed with its
`publish.sh`; the repository's README says how it is checked and uploaded.
