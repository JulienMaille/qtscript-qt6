# Porting notes

## Upstream source

- Repository: `https://invent.kde.org/qt/qt/qtscript.git`
- Release branch: `5.15.19`

The repository stores patch files instead of an upstream source snapshot. The
Windows and Linux scripts clone the QtScript release, apply the files in
`patches/` in lexical order, and optionally apply the selected test changes in
`patches/optional/tests` with `-IncludePortedTests`.

The scripts reuse an existing clean source directory that has already been
patched. Delete that directory under `.work/` before applying a changed patch
series.

## Default patch series

1. `0001-Add-minimal-Qt-6-CMake-core-build.patch` adds the CMake module
   definition and explicit source/header manifests for the interpreter build.
2. `0002-Port-JavaScriptCore-subset-to-C-17.patch` contains the C++17/MSVC
   adaptations exercised by that source manifest.
3. `0003-Adapt-QtScript-API-and-metatypes-to-Qt-6.patch` handles Qt 6 API,
   container, enum/metatype, atomic, and date/time differences.
4. `0004-Adapt-QObject-bridge-to-Qt-6.patch` contains the QObject, Qt 6
   metaobject, method invocation, property, and signal bridge changes.
5. `0005-Replace-removed-QBoolBlocker-helper.patch` replaces the removed
   private Qt helper with `QScopedValueRollback<bool>`.
6. `0006-Remove-Core5Compat-dependency.patch` supplies the legacy public
   `QRegExp` API using `QRegularExpression`, preserves key Qt 5 regexp
   behavior, and removes the Core5Compat dependency.
7. `0007-Add-Linux-core-build-support.patch` selects the platform-specific
   JavaScriptCore stack allocator for Linux builds.
8. `0008-Promote-INT32_MIN-negation-to-double-in-negate-opcode.patch` fixes
   `QTBUG-32829`: signed overflow when negating the smallest 32-bit integer.

## Optional test layer

`patches/optional/tests` updates selected upstream tests and is applied with
`-IncludePortedTests`. The normal module build keeps `QT_BUILD_TESTS=OFF`.
The optional test layer is not required to compile or smoke-test the core
module.

The default external smoke test covers the QtScript API, `QVariant`, and the
ported `QRegExp` behavior, including escaped Unix wildcards, greedy and
minimal matching, captures, and clearing match state after failed searches.

## Build scope

The Qt 6 CMake entry point currently builds the core `Script` module only. It
exports `Qt6::Script`, public/private headers, and CMake package metadata.
Examples, documentation, qmake integration, x86, platforms other than Windows
and Linux, and ScriptTools are outside the current acceptance scope.

The source defaults to package version 6.8.3, while the build scripts read the
selected installation with `qtpaths` and supply its actual version to CMake.
