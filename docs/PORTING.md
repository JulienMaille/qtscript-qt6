# Porting notes

## Upstream source

- Repository: `https://invent.kde.org/qt/qt/qtscript.git`
- Release branch: `5.15.19`

The repository stores patches, not a snapshot of the upstream sources. The
Windows and Linux scripts clone the QtScript release, apply the files in
`patches/quickjs/` in lexical order, and optionally apply the selected test
changes in `patches/optional/tests/` with `-IncludePortedTests`. The legacy
JavaScriptCore port remains under `patches/` for the non-QuickJS line.

The scripts reuse an existing clean source directory that has already been
patched. Delete that directory under `.work/` before applying a changed patch
series.

## CMake entry point

`cmake/` mirrors the Qt 6 build files at their in-tree paths
(`CMakeLists.txt`, `.cmake.conf`, `src/CMakeLists.txt`,
`src/script/CMakeLists.txt`, `src/scripttools/CMakeLists.txt`) for review; the
QuickJS-NG CMake files are carried by `patches/quickjs/0001`. The module never
depends on QtCore5Compat. The legacy `QRegExp` compatibility API is compiled
in by default and can be disabled with `-DSCRIPT_QREGEXP=OFF`, which defines
`QT_NO_REGEXP` and drops the `QtScript/QRegExp` header.

## QuickJS-NG patch series

The ordered files in `patches/quickjs/` form the migration line:

1. `0001` replaces JavaScriptCore with the pinned QuickJS-NG backend and
   carries the Qt 6 CMake/module entry points.
2. `0002` ports the ScriptTools shell; `0003`–`0006` advance public API,
   QVariant, QObject, global-object, accessor, and ScriptTools compatibility.
3. `0007` adds bounded evaluation, context frames, and runtime robustness.
4. `0008` fixes QObject wrapper ownership, GC bookkeeping, and teardown safety.
5. `0009`–`0010` preserve QRegExp caret behavior across alternatives.
6. `0011` queues cross-thread QObject signals onto the engine thread so
   QuickJS remains single-threaded without dropping signal delivery.
7. `0012` defers QObject destruction until after QuickJS garbage collection;
   `0013` carries the current compatibility and context-bridge fixes.

The pinned QuickJS-NG source is kept as a submodule. The small patch in
`patches/quickjs-ng/` adds the host hooks required by the QtScript bridge; the
QuickJS build scripts apply it idempotently after checking the pinned revision.

## Optional test layer

`patches/optional/tests` updates selected upstream tests and is applied with
`-IncludePortedTests`. The normal module build keeps `QT_BUILD_TESTS=OFF`.
The optional test layer is not required to compile or smoke-test the core
module.

## Build scope

The Qt 6 CMake entry point builds the `Script` module and the `ScriptTools`
debugger module (`Qt6::ScriptTools`, including the `QScriptEngineDebugger`
widget and the `scripttools_debugging` resources). It exports `Qt6::Script`
and `Qt6::ScriptTools`, their public/private headers, and CMake package
metadata. Examples, documentation, qmake integration, x86, and platforms
other than Windows and Linux are outside the acceptance scope.
