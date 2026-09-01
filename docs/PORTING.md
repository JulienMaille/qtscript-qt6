# Porting notes

## Upstream source

- Repository: `https://invent.kde.org/qt/qt/qtscript.git`
- Release branch: `5.15.19`

The repository stores patches, not a snapshot of the upstream sources. Files
that no patch may own — the QuickJS-NG backend sources, the Qt 6 CMake entry
points, and the ported-test CMake aggregators — are kept as plain files under
[`overlay/`](../overlay) and copied into the source tree by the build scripts
before the series is applied. The Windows and Linux scripts clone the QtScript
release, copy the overlay, apply the files in `patches/quickjs/` in lexical
order, and optionally apply the selected test changes in
`patches/optional/tests/` with `-IncludePortedTests`. The QuickJS-NG series is
the only supported backend on this branch.

The scripts reuse an existing clean source directory that has already been
patched. Delete that directory under `.work/` before applying a changed patch
or overlay file.

## CMake entry point

The Qt 6 CMake entry points (`.cmake.conf`, the root `CMakeLists.txt`, and the
`src/`, `src/script/`, `src/scripttools/` lists) are overlay files copied by
the build scripts; no patch creates or edits them. The module never depends on
QtCore5Compat. The legacy `QRegExp` compatibility API is compiled in by
default and can be disabled with `-DSCRIPT_QREGEXP=OFF`, which defines
`QT_NO_REGEXP` and drops the `QtScript/QRegExp` header.

## QuickJS-NG patch series

The migration line is split between the `overlay/` files and the ordered
patches in `patches/quickjs/`.

`overlay/` carries the complete QuickJS-NG backend (`src/script/quickjs/`,
`src/script/api/qregexp.*`, the ScriptTools shell
`src/scripttools/debugging/qscriptenginedebuggerquickjs.cpp`) at its final,
modernized state. The backend evolution that previously advanced it —
QVariant and QObject marshalling, global-object and accessor compatibility,
bounded execution and context frames, wrapper ownership, cross-thread
signals, QRegExp caret modes, JSC-shim removal, registered-payload fast
paths, the Clang build fix, iterator and evaluate fixes, prototype and
exception preservation, and native-argument arrays — is baked into those
files, so no patch needs to create or rewrite them.

The remaining patches only touch upstream files that exist in the 5.15.19
release:

1. `0001` adapts the public API headers (`qscriptengine.h`,
   `qscriptvalue.h`) to the QuickJS-NG backend declared by the overlay.
2. `0002` adapts the ScriptTools debugger header to the overlay shell.
3. `0003` drops JSC-specific expected-failure annotations from
   `tst_qscriptvalue.cpp`.
4. `0004` skips QuickJS-incompatible tests and expects the unicode-test
   `length` deviation.
5. `0005` skips QuickJS enumeration and arguments-object deviations in the
   qscriptcontext and qscriptextqobject suites.

The pinned QuickJS-NG source is kept as a submodule. The ordered patches in
`patches/quickjs-ng/` add the host hooks required by the QtScript bridge:
`0005` fixes direct `eval()` method receivers inside a captured `with` scope;
`0006` removes the non-standard read-only-prototype shadowing switch; and
`0007` removes malformed string-escape and unresolved-label parser shims.
The QuickJS build scripts apply them idempotently after checking the pinned
revision. The bridge diagnostics they condition are standard QuickJS output:
the overlay backend emits no JSC-style error-message normalization, restores
the standard RegExp constructor semantics, keeps QObject pointer wrappers
reusable with legacy normalized signal signatures, and makes native QVariant
conversion independent of generated QObject prototypes, which also removes
the need for module-specific connection-name markers in QSqlDatabase
bindings.

## Optional test layer

`patches/optional/tests` updates selected upstream tests and is applied with
`-IncludePortedTests`. This series is deliberately test-only: runtime and
bridge changes belong in the overlay backend, so a clean QtScript checkout
can apply the optional tests after copying the overlay without replaying
superseded implementation hunks. Every ported suite's `CMakeLists.txt`
aggregator — top-level and per-suite — is an overlay file; `0003` additionally
removes the obsolete `tests/auto/cmake` aggregator. `0007` is
the broad conformance modernization;
`0008` removes obsolete property, malformed-escape, and unresolved-label
expectations; `0009` updates built-in function-length assignments; `0010`
removes a stale XFAIL that had become an XPASS; `0011` removes the reserved-word
source-rewrite shim and updates property/object-literal expectations; and
`0012` removes duplicate-RegExp-flag normalization; `0013` corrects a stale
Unicode resource-length assertion; `0014` and `0015` modernize error-message
expectations; `0016` accepts standard RegExp constructor behavior; `0017`
modernizes the corresponding ECMAScript-3 conformance case; and `0018` updates
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
