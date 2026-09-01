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
11. `0024` makes `QScriptValueIterator` yield non-enumerable own properties
    (the array/string `length` property and user properties flagged with
    `QScriptValue::SkipInEnumeration` are still visited, with the bit reported
    through `flags()`); `0025` drops the reserved-word rewrite shim from
    `QScriptEngine::evaluate()` so QuickJS reports the true parse result both
    for valid reserved-word property names (`o.break = 123`, `{ break: 123 }`)
    and for invalid declarations. `0026` compares `QVariant` wrappers by value
    in QuickJS object equality so two wrappers of equal variants compare equal;
    `0027` honors a registered `QScriptClass` in `QScriptValue::toVariant()`
    instead of extracting the raw payload, and guards a detached (null-state)
    left operand in `QScriptValue::equals()`.
12. `0028` keeps undeclared global assignments on the runtime global object
    under a custom activation scope (only declared names are copied to the
    activation object) and makes `QScriptEngine::importExtension()` load the
    per-extension `__init__.js` from a compiled-in
    `:/qtscriptextension/<prefix>` resource before the library-path scan, so
    static extensions run their init script; the sloppy-mode contract matters
    here because extension init scripts install their globals through
    undeclared assignments. `0029` reports scriptable `QObject` `Q_PROPERTY`s
    from `get_own_property` as accessor properties so inherited assignment
    invokes the property setter instead of shadowing the wrapper; the
    descriptor carries the same enumerable flag the wrapper shape installs
    for meta- and dynamic properties, so the exotic hook never disagrees
    with the shape; `0030` applies the metatype-registered dynamic-class
    prototype to reused `QObject` wrappers so a returned `QObject` value is
    not down-graded to a base-class prototype — the upgrade is applied only
    while the wrapper still carries an engine-assigned prototype, so a
    user-assigned prototype survives later conversions; `0031` preserves a
    pending exception across
    `evaluate()` scope cleanup; `0032` builds native-function argument arrays
    as real JS arrays; `0033` skips QuickJS-incompatible tests and expects
    the unicode-test `length` deviation; `0034` skips QuickJS enumeration and
    arguments-object deviations in the qscriptcontext and qscriptextqobject
    suites.

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

## macOS-only patch layer

`patches/macos` carries JSC fixes needed only by Apple Clang on macOS and is
applied by `build-macos.sh` (as `--include-macos`) after the default series:

1. `0009-Drop-the-obsolete-macOS-ceil-workaround.patch` removes the
   `#define ceil(x) wtf_ceil(x)` libc workaround from `MathExtras.h`; the
   function-like macro rewrites `std::ceil` inside libc++ private headers
   and breaks the Apple Clang 17/libc++ compile.
2. `0010-Support-Apple-Silicon-AArch64-in-the-conservative-stack-scanner.patch`
   adds the `arm_thread_state64_t`/`ARM_THREAD_STATE64` branches to
   `Collector.cpp` so the garbage collector can suspend and scan helper
   threads on Apple Silicon.
3. `0011-Drop-the-CoreFoundation-dependency-of-the-Darwin-time-implementation.patch`
   replaces `CFAbsoluteTimeGetCurrent` in `CurrentTime.cpp` with the
   `gettimeofday` implementation used by the file's POSIX branch; the module
   does not link CoreFoundation and the x86_64 slice of the universal
   framework otherwise fails to link.

4. `0012-Restore-the-single-threaded-JSLock-build-on-macOS.patch` defines
   `ENABLE_JSC_MULTIPLE_THREADS=0` on Darwin like the qmake build's
   `script.pro` did. Without it the CMake build used real thread-local
   lock counting while the API never takes the JSLock, and
   `Heap::allocate`'s `JSLock::lockCount() > 0` assertion crashed every
   `QScriptEngine` construction in Debug builds.

5. `0013-Canonicalize-the-script-library-path-in-QScriptEngine-availableExtensions.patch`
   canonicalizes the `script` library subdirectory in
   `QScriptEngine::availableExtensions()` after confirming it exists. On
   macOS `/tmp` and `/var` are symlinks into `/private`, so a process
   launched from `/tmp` would compare a `/tmp`-rooted library path against
   `/private/tmp`-resolved entries and report mismatched extension paths.
   GitHub runners do not expose this (`/Users/runner` is not a symlink).

The Windows and Linux scripts never pass `--include-macos`, so the default
series they apply is unchanged.

## Build scope

The Qt 6 CMake entry point builds the `Script` module and the `ScriptTools`
debugger module (`Qt6::ScriptTools`, including the `QScriptEngineDebugger`
widget and the `scripttools_debugging` resources). It exports `Qt6::Script`
and `Qt6::ScriptTools`, their public/private headers, and CMake package
metadata. Examples, documentation, qmake integration, x86, and platforms
other than Windows, Linux, and macOS Apple Silicon are outside the acceptance
scope.
