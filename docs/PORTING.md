# Porting notes

## Upstream source

- Repository: `https://invent.kde.org/qt/qt/qtscript.git`
- Release branch: `5.15.19`

The repository stores patches, not a snapshot of the upstream sources. The
Windows and Linux scripts clone the QtScript release, apply the files in
`patches/quickjs/` in lexical order, and optionally apply the selected test
changes in `patches/optional/tests/` with `-IncludePortedTests`. The QuickJS-NG
series is the only supported backend on this branch.

The scripts reuse an existing clean source directory that has already been
patched. Delete that directory under `.work/` before applying a changed patch
series.

## CMake entry point

The QuickJS-NG CMake files are carried by `patches/quickjs/0001`; there is no
second, backend-specific CMake mirror in this branch. The module never depends
on QtCore5Compat. The legacy `QRegExp` compatibility API is compiled in by
default and can be disabled with `-DSCRIPT_QREGEXP=OFF`, which defines
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
   `0013`–`0016` carry the current compatibility and context-bridge fixes.
8. `0017`–`0018` remove JSC-style diagnostic and RegExp language shims;
   `0019` keeps QObject pointer wrappers reusable and accepts legacy
   normalized signal signatures such as `valueChanged(const QString&)`.
9. `0020` adds a shared native QVariant payload fast path; `0021` hardens
   payload extraction for nested evaluation and never invokes JavaScript
   marker accessors while converting native arguments.
10. `0022` fixes the portable C++17 spelling of the signed-char QVariant
    conversion used by both public conversion paths; `0023` uses QuickJS-NG's
    function-pointer union and removes an unused result so the backend builds
    cleanly with Clang's strict function-cast and warning diagnostics.

The pinned QuickJS-NG source is kept as a submodule. The ordered patches in
`patches/quickjs-ng/` add the host hooks required by the QtScript bridge.
`0005` fixes direct `eval()` method receivers inside a captured `with` scope;
`0006` removes the non-standard read-only-prototype shadowing switch; and
`0007` removes malformed string-escape and unresolved-label parser shims;
The runtime patches `patches/quickjs/0017` and `0018` remove JSC-style
error-message normalization and restore the standard QuickJS RegExp constructor
semantics. `0019` restores the QObject bridge behavior required by legacy
QtScript applications without reintroducing JSC semantics. `0020`–`0021`
make native QVariant conversion independent of generated QObject prototypes;
this also removes the need for module-specific connection-name markers in
QSqlDatabase bindings. The QuickJS build scripts apply all of these patches
idempotently after checking the pinned revision.

## Optional test layer

`patches/optional/tests` updates selected upstream tests and is applied with
`-IncludePortedTests`. This series is deliberately test-only: runtime and
bridge changes belong in the ordered `patches/quickjs/` series, so a clean
QtScript checkout can apply the optional tests after the complete QuickJS
port without replaying superseded implementation hunks. `0008` is the broad
conformance modernization;
`0009` removes obsolete property, malformed-escape, and unresolved-label
expectations; `0010` updates built-in function-length assignments; `0011`
removes a stale XFAIL that had become an XPASS; `0012` removes the reserved-word
source-rewrite shim and updates property/object-literal expectations; and
`0013` removes duplicate-RegExp-flag normalization; `0014` corrects a stale
Unicode resource-length assertion; `0015` and `0016` modernize error-message
expectations; `0017` accepts standard RegExp constructor behavior; `0018`
modernizes the corresponding ECMAScript-3 conformance case; and `0019` updates
the QObject deleted-call diagnostic to the native QuickJS error. These
patches modernize
assertions that only described obsolete V8/JSC behavior; they do not add
runtime shims for those quirks. The normal module build keeps
`QT_BUILD_TESTS=OFF`. The optional test layer is not required to compile or
smoke-test the core module.

## Build scope

The Qt 6 CMake entry point builds the `Script` module and the `ScriptTools`
debugger module (`Qt6::ScriptTools`, including the `QScriptEngineDebugger`
widget and the `scripttools_debugging` resources). It exports `Qt6::Script`
and `Qt6::ScriptTools`, their public/private headers, and CMake package
metadata. Examples, documentation, qmake integration, x86, and platforms
other than Windows and Linux are outside the acceptance scope.
