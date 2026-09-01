# Compatibility test summary

## Current core verification

The smoke test covers evaluation, function calls, exceptions, `QVariant`, and
`QRegExp` compatibility: round-tripping, captures, fixed strings, escaped
`WildcardUnix` patterns, greedy `RegExp` and `RegExp2` matching, explicit
minimal matching, and match-state clearing after failed searches. It also
covers QObject exposure, enum property/invokable conversion, and signals.
The build drivers check that the installed QtScript binary does not link
Core5Compat/Qt5Compat.

The QObject bridge supports the Qt 6.8 and Qt 6.11 `moc` layouts. The
metaobject code is exercised in CI on the Qt 6.8.3 LTS (Linux GCC, Windows
MSVC 2022, macOS Apple Clang) and Qt 6.11 (Linux GCC, Windows MSVC 2026,
macOS Apple Clang) legs.

The optional test layer is compiled on every CI matrix job and executed via
`ctest` in a dedicated step on each Debug job. Release jobs compile the suites
but validate via build, link/load, and the smoke test instead of running them;
macOS additionally validates that the installed frameworks are universal
(`arm64;x86_64`), contain no AGL or Core5Compat linkage, and survive archive
round-trips preserving their symlinks; see
[Current-matrix re-validation](#current-matrix-re-validation) below.

## Historical full-series results

These results come from the earlier full compatibility series, measured on
Qt 6.9.2, Windows x64, MSVC 2022, before the test port was wired into CI.
They justify retaining enum compatibility in the core port and the selected
test port under `patches/optional/tests`. The comparison baseline was the
existing Qt 5.15.19 KDE x64 installation and untouched QtScript source.

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
Qt 6.9.2, Windows x64, MSVC 19.44 (VS 2022 Professional), Ninja
Multi-Config. All thirteen suites are wired into the `tests/CMakeLists.txt`
aggregator carried by `patches/optional/tests/0003` and
`patches/optional/tests/0005`; every CI leg builds them
(`-IncludePortedTests`), and each Debug leg executes them through a dedicated
`ctest` step. Running them locally is a `ctest` call after the build; see
[Reproducing the matrix](#reproducing-the-matrix). Both Debug and Release
configurations were measured locally through `ctest`.

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
| ECMAScript (Qt 6) | 29,890 | 0 | 147 | Pass |
| V8 (Qt 6) | 137 | 0 | 7 | Pass |

The two fixes are:

- `patches/0007` removes the `INT32_MIN` negation overflow in the
  interpreter's negate opcode (signed-overflow UB; `-(-2147483648)` promotes
  to the double 2^31 per the specification). This was the cause of
  the `-(-2147483648)`/`- -"0x80000000"` failures on GCC Debug and their
  MSVC64/MINGW64 XFAIL records (QTBUG-32829).
- `patches/optional/tests/0004` drops those now-obsolete XFAIL entries from
  `expect_fail.txt`; the three tests pass on every toolchain.

One test expectation was updated in this re-validation
(`patches/optional/tests/0002-…`): a `QScriptEngine*` signal argument is
wrapped as a QObject instead of reported as an unregistered datatype, since
Qt 6 registers QObject-derived pointer metatypes implicitly.

### Reproducing the matrix

The suites are not vendored: the build scripts clone QtScript 5.15.19 and apply
`patches/` and `patches/optional/tests/` when `-IncludePortedTests` is passed.
They are then run via `ctest` against the build tree. On Windows:

```powershell
$work = Join-Path (Get-Location) '.work\6.9.2\Debug'
.\scripts\build-windows.ps1 -QtRoot C:\Qt\6.9.2\msvc2022_64 -WorkRoot $work -Configuration Debug -IncludePortedTests
ctest --test-dir (Join-Path $work 'build') -C Debug --output-on-failure
```

The same build with `-Configuration Release` reproduces the Release
measurement. On Linux:

```bash
work_root="$PWD/.work/6.9.2/Debug"
./scripts/build-linux.sh --qt-root "$HOME/Qt/6.9.2/gcc_64" --work-root "$work_root" --configuration Debug --include-ported-tests
ctest --test-dir "$work_root/build" --output-on-failure
```

On macOS, install into an isolated prefix before running the suites:

```bash
work_root="$PWD/.work/6.9.2/Debug"
install_prefix="$work_root/install"
./scripts/build-macos.sh --qt-root "$HOME/Qt/6.9.2/macos" \
  --work-root "$work_root" --install-prefix "$install_prefix" \
  --configuration Debug --include-ported-tests
ctest --test-dir "$work_root/build" --output-on-failure
```

The same build with `--configuration Release` reproduces the Release
measurement; the Release job additionally archives the install tree and
verifies the framework symlinks survive the round-trip.

In CI, every matrix job compiles the suites (`-IncludePortedTests`); each Debug
leg then executes them via a dedicated `ctest` step, so test failures surface
separately from the module build.
