# Compatibility test summary

## Current core verification

The current core patch series was applied to a fresh QtScript 5.15.19 checkout.
The module and an external CMake consumer then built and passed in both MSVC
x64 configurations:

| Configuration | Module | External smoke test |
|---|---|---|
| Release | Pass | 1/1 passed |
| Debug | Pass | 1/1 passed |

The smoke test covers evaluation, function calls, exceptions, `QVariant`, and
`QRegExp` compatibility: round-tripping, captures, fixed strings, escaped
`WildcardUnix` patterns, greedy `RegExp` and `RegExp2` matching, explicit
minimal matching, and match-state clearing after failed searches. It also
covers QObject exposure, enum property/invokable conversion, and signals.
Installed CMake/qmake metadata was also scanned and contained no paths into the
source or build directories. The build drivers also verify that the installed
metadata and QtScript binary contain no Core5Compat or Qt5Compat dependency.

The QObject bridge supports the Qt 6.8 and Qt 6.11 `moc` layouts. The
metaobject code is exercised in CI on the Qt 6.8.3 LTS (Linux GCC, Windows
MSVC 2022) and Qt 6.11 (Linux GCC, Windows MSVC 2026) legs.

The optional test layer is compiled on every CI matrix job and executed via
`ctest` on each Debug job. Release jobs compile the suites but use build,
link/load, and smoke validation rather than treating the upstream suites as a
Release gate; see [Current-matrix re-validation](#current-matrix-re-validation)
below.

## Historical full-series results

These results were collected from the earlier full compatibility series against
Qt 6.9.2, Windows x64, MSVC 2022. They justify retaining enum compatibility in
the core port and the selected test port under `patches/optional/tests`; they
do not imply that the default CI builds the upstream suites. The comparison
baseline was the existing Qt 5.15.19 KDE x64 installation and untouched
QtScript source.

| Suite | Passed | Failed | Skipped | Result |
|---|---:|---:|---:|---|
| qscriptengine | 522 | 0 | 2 | Pass |
| qscriptcontext | 75 | 0 | 1 | Pass |
| qscriptclass | 24 | 0 | 0 | Pass |
| qscriptstring | 23 | 0 | 0 | Pass |
| qscriptvalue | 374 | 0 | 0 | Pass |
| qscriptvalueiterator | 21 | 0 | 0 | Pass |
| qscriptextqobject | 79 | 0 | 0 | Pass |
| qscriptextensionplugin | 4 | 0 | 0 | Pass |
| qscriptable | 8 | 0 | 0 | Pass |
| qscriptcontextinfo | 10 | 0 | 0 | Pass |
| qscriptqwidgets | 4 | 0 | 0 | Pass |
| ECMAScript (Qt 6) | 29,888 | 2 | 147 | Baseline-compatible |
| ECMAScript (Qt 5) | 29,885 | 5 | 147 | Baseline |
| V8 (Qt 6) | 136 | 1 | 7 | Baseline-compatible |
| V8 (Qt 5) | 136 | 1 | 7 | Baseline |

The two Qt 6 ECMAScript failures are operand-evaluation-order defects also
present in Qt 5. Qt 6 eliminates three other failures observed in the Qt 5
baseline. The V8 `negate` failure is identical in both builds.

## Current-matrix re-validation

Re-run of the ported upstream suites on the current checkout, measured locally on
Qt 6.8.3 LTS, Windows 11 x64, MSVC 19.51 (VS 2026 Community), Ninja
Multi-Config. The six suites are wired into a `tests/CMakeLists.txt`
aggregator carried by `patches/optional/tests/0003`; every CI leg now builds
them (`-IncludePortedTests`). The Debug leg of each OS executes them via
`ctest`; the Release legs keep the build + smoke test, because the Release
suites consistently show the three historical optimizer-dependent failures
(see below).

| Suite | Passed | Failed | Skipped | Result |
|---|---:|---:|---:|---|
| qscriptengine | 522 | 0 | 2 | Pass |
| qscriptcontext | 75 | 0 | 1 | Pass |
| qscriptvalue | 374 | 0 | 0 | Pass |
| qscriptextqobject | 79 | 0 | 0 | Pass |
| ECMAScript (Qt 6) | 29,890 | 0 | 147 | Pass |
| V8 (Qt 6) | 137 | 0 | 7 | Pass |

(measured Debug, Qt 6.8.3 LTS, MSVC 2026, Ninja Multi-Config; the Release
build shows the same 29,890/0/147 and 137/0/7)

These zero-failure totals cover executed cases; 147 ECMAScript cases and 7 V8
cases remain skipped.

An initial Release run showed the historical totals (ECMAScript
29,888/2/147, V8 136/1/7): three failures asserting on the order of JavaScript
operand `valueOf()` side effects plus the `-2147483648` literal, which the
optimizer evaluates differently per build flags. Two fixes landed:

- `patches/0008` removes the `INT32_MIN` negation overflow in the
  interpreter's negate opcode (signed-overflow UB; `-(-2147483648)` now
  promotes to the double 2^31 per the specification). This was the cause of
  the `-(-2147483648)`/`- -"0x80000000"` failures on GCC Debug and their
  MSVC64/MINGW64 XFAIL records (QTBUG-32829).
- `patches/optional/tests/0004` drops those now-obsolete XFAIL entries from
  `expect_fail.txt`; the three tests pass on every toolchain.

With both fixes the suites are green in Debug and Release on Qt 6.8.3/MSVC
2026 (29,890/0/147 and 137/0/7). CI runs the suites on the Debug leg of each
OS and keeps the Release legs as build/link/load verification: the two
remaining evaluation-order tests (`ecma_3/Operators/order-01.js` 11.5.1 and
11.13.2, V8 `negate`) are codegen-sensitive — the vendored `tests/negate.js`
itself comments that the correct evaluation order "is not implemented by any
of the known JS engines" — so their outcome is not treated as a port-quality
signal.

One test expectation was ported as part of this re-validation
(`patches/optional/tests/0002-…`): a `QScriptEngine*` signal argument is now
wrapped as a QObject instead of reported as an unregistered datatype, because
Qt 6 registers QObject-derived pointer metatypes implicitly.

The default minimal series is independently built and smoke-tested through its
CMake package in Debug and Release. qmake consumer validation remains deferred;
the `.pri` files emitted by Qt's module machinery are not part of the supported
interface yet.
