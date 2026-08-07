# Compatibility test summary

## Current minimal-series verification

On 2026-08-07, the seven-patch default series was applied to a fresh clone and
verified at tree `63bf6ef09faee052851a504425d00f3ea105bbfd`. The module and an
external CMake consumer then built and passed in both MSVC x64 configurations:

| Configuration | Module | External smoke test |
|---|---|---|
| Release | Pass | 1/1 passed |
| Debug | Pass | 1/1 passed |

The smoke test covers evaluation, function calls, exceptions, `QVariant`,
`QRegExp` regular-expression, wildcard, fixed-string and capture behavior,
QObject exposure, enum property/invokable conversion, and signals.
Installed CMake/qmake metadata was also scanned and contained no paths into the
source or build directories. The Release install was additionally
verified to contain no Core5Compat/Qt5Compat metadata references or DLL import.

The optional test layer applied cleanly on top and produced its recorded tree
identifier. It was not compiled as part of this minimal-series verification.

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

The default minimal series is independently built and smoke-tested through its
CMake package in Debug and Release. qmake consumer validation remains deferred;
the `.pri` files emitted by Qt's module machinery are not part of the supported
interface yet.
