# QtScript for Qt 6

[![Windows Qt 6.8 LTS](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-lts.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-lts.yml)
[![Windows Qt 6.11](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-latest.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-latest.yml)
[![Linux Qt 6.8 LTS](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-lts.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-lts.yml)
[![Linux Qt 6.11](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-latest.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-latest.yml)
[![macOS Qt 6.8 LTS](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/macos-lts.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/macos-lts.yml)
[![macOS Qt 6.11](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/macos-latest.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/macos-latest.yml)

> [!IMPORTANT]
> A modernized QtScript ditching JSC in favor of QuickJS-NG is work-in-progress in the following branch https://github.com/JulienMaille/qtscript-qt6/tree/quickjs-modernize

> [!NOTE]
> Looking for a compatible port of [qtscriptgenerator](https://github.com/JulienMaille/qtscriptgenerator-qt6)?

This repository provides the patches needed to build the QtScript core
module and the ScriptTools debugger module with Qt 6 on Windows x64/MSVC,
Linux x64/GCC, and macOS Apple Silicon/Apple Clang. Qt 6.8 LTS (6.8.3) is the
baseline. CI also covers the latest 6.11.x on all three platforms.

QtScript source is not vendored. The build script clones KDE's QtScript
5.15.19 revision, copies the QuickJS-NG backend and the Qt 6 CMake entry
points from [`overlay`](overlay), applies the ordered patch files in
[`patches/quickjs`](patches/quickjs) (and optionally
[`patches/optional/tests`](patches/optional/tests)), and builds the modules.

## Status

- Builds Debug and Release variants of `Qt6::Script` and `Qt6::ScriptTools`.
- Installs alongside the other Qt modules, like the Qt 5 module did.
- Preserves the public `QScript*` source API. Qt 5 binary compatibility is not
  supported.
- Ports the ScriptTools debugger (`QScriptEngineDebugger`, script/console/
  breakpoint widgets, completion and error reporting) with its
  `scripttools_debugging` resources.
- Does not depend on Core5Compat.
- Uses the pinned QuickJS-NG 0.16.1 source as a static engine; shared QuickJS
  runtime libraries are rejected by the build checks.
- Passes more than 20 checks in the external CMake smoke test: evaluation,
  calls, exceptions, `QVariant`, `QRegExp` compatibility, QObject exposure
  with enum conversion, ownership/GC, same-thread/cross-thread signals, and
  nested evaluation of native QVariant wrappers. See
  [`docs/VALIDATION.md`](docs/VALIDATION.md) for full results.
- CI builds the ported upstream suites on every matrix job and executes
  them via a dedicated `ctest` step on each Debug job.
- Pushes to `quickjs-modernize` run the same four LTS/latest Linux/Windows
  jobs and publish their Release artifacts as a per-commit GitHub prerelease
  (`quickjs-modernize-<short SHA>`). Stable `v*` releases from `main` retain
  the existing release workflow. The branch prerelease gate requires all four
  Release jobs and smoke tests; Debug inherited-suite failures remain visible
  in CI without blocking those explicitly marked experimental artifacts.
- Fixes the inherited QtScript `INT32_MIN` negation bug tracked as `QTBUG-32829`.

Legacy `QRegExp` signatures use `QtScript/QRegExp`, implemented with Qt 6
`QRegularExpression`. Regular-expression, wildcard, fixed-string, capture, and
replacement behaviors are supported. This compatibility API is compiled in by
default and can be disabled with `-DSCRIPT_QREGEXP=OFF`.

See [`docs/PORTING.md`](docs/PORTING.md) for the patch inventory.

> This port was developed with AI assistance under human planning and review,
with every change verified by continuous integration.

## Requirements

- CMake 3.16 or newer; qmake is not supported.
- Windows: Visual Studio 2022 or newer with the MSVC x64 C++ toolchain
  (CMake auto-detects the newest installed; CI exercises MSVC 2022 on the
  LTS leg and MSVC 2026 on the latest-Qt leg).
- Linux: GCC (C++17) and Ninja.
- macOS: Apple Clang and Ninja on Apple Silicon.
- A supported Qt 6.8 through Qt 6.11 installation with private module build tooling
  (`qt-cmake-private`, `qtpaths`).

## Build

On Windows:

```powershell
git submodule update --init third_party/quickjs-ng
.\scripts\build-quickjs-ng.ps1 -Configuration Release
.\scripts\build-windows.ps1 -QtRoot C:\Qt\6.8.3\msvc2022_64 -Configuration Release
```

On Linux:

```bash
git submodule update --init third_party/quickjs-ng
bash ./scripts/build-quickjs-ng.sh --configuration Release
./scripts/build-linux.sh --qt-root "$HOME/Qt/6.8.3/gcc_64" --configuration Release
```

The QuickJS step builds the pinned static engine. The QtScript script then
clones the 5.15.19 sources, copies the overlay files, applies the patch
series, builds, and installs both
modules into the Qt prefix using the same layout the Qt 5
module used: headers under `include\QtScript` and `include\QtScriptTools`,
CMake packages under `lib\cmake\Qt6Script` and `lib\cmake\Qt6ScriptTools`, binaries in
`bin`/`lib`, and module registration under `mkspecs\modules`. It then runs an
out-of-tree CMake smoke build that compiles and runs against the installed
modules.

## Security

QtScript embeds a legacy QuickJS-NG runtime. It is not a security sandbox; run
only trusted scripts and never expose it to untrusted content.

## License

QtScript and patched source retain their upstream licenses. License texts are
included in [`LICENSES`](LICENSES).
