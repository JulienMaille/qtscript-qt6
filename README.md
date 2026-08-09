# QtScript for Qt 6

[![Windows Qt 6.8 LTS](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-lts.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-lts.yml)
[![Windows Qt 6.11](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-latest.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-latest.yml)
[![Linux Qt 6.8 LTS](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-lts.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-lts.yml)
[![Linux Qt 6.11](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-latest.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux-latest.yml)

This repository provides the patches needed to build the QtScript core module
with Qt 6 on Windows x64/MSVC and Linux x64/GCC. Qt 6.8 LTS (6.8.3) is the
baseline; CI also covers the latest 6.11.x on both platforms.

QtScript source is not vendored. The build script clones KDE's QtScript
5.15.19 revision and applies the ordered files in [`patches`](patches).

## Status

- Builds Debug and Release variants of `Qt6::Script`.
- Installs into the Qt installation next to the other modules, like the
  Qt 5 module did.
- Preserves the public `QScript*` source API. Qt 5 binary compatibility is not
  supported.
- Does not depend on Core5Compat.
- Tests evaluation, calls, exceptions, `QVariant`, and `QRegExp` compatibility,
  including escaped Unix wildcards, greedy `RegExp`/`RegExp2` matching, minimal
  matching, captures, and failed-match state, plus QObject exposure, enum
  conversion and signals.
- CI builds the six ported upstream suites on every matrix job and executes them
  on each Debug job. Release jobs provide build, link/load, and smoke validation;
  detailed results and known skips are recorded in
  [`docs/VALIDATION.md`](docs/VALIDATION.md).
- Fixes the inherited QtScript `INT32_MIN` negation bug tracked as `QTBUG-32829`.

Legacy `QRegExp` signatures use `QtScript/QRegExp`, implemented with Qt 6
`QRegularExpression`. Regular-expression, wildcard, fixed-string, capture and
replacement behavior is supported.

See [`PORTING.md`](docs/PORTING.md) for the patch inventory and
[`docs/VALIDATION.md`](docs/VALIDATION.md) for test results.

This port was developed with AI assistance under human planning and review,
with every change verified by continuous integration.

## Requirements

- CMake 3.16 or newer.
- Windows: Visual Studio 2022 or newer with the MSVC x64 C++ toolchain
  (CMake auto-detects the newest installed; CI exercises MSVC 2022 on the
  LTS leg and MSVC 2026 on the latest-Qt leg).
- Linux: GCC (C++17), Ninja and Bash.
- A Qt 6.8 LTS or newer installation (CI also covers 6.11.x) with private
  module build tooling (`qt-cmake-private`, `qtpaths`).

## Build

On Windows:

```powershell
.\scripts\build-windows.ps1 -QtRoot C:\Qt\6.8.3\msvc2022_64 -Configuration Release
```

The script clones and patches QtScript, builds it, installs it into the Qt
installation next to the other modules (the same layout Qt 5's module used:
`include\QtScript`, `lib\cmake\Qt6Script`, the library in `bin`/`lib`, and
`mkspecs\modules\qt_lib_script.pri`), and runs an external CMake smoke test.

On Linux:

```bash
./scripts/build-linux.sh --qt-root "$HOME/Qt/6.8.3/gcc_64" --configuration Release
```

CMake consumption is tested. qmake consumption is not currently supported.

## Security

QtScript embeds a legacy 2011 JavaScriptCore snapshot. It is not a security
sandbox; run only trusted scripts. See [`SECURITY.md`](SECURITY.md).

## License

QtScript and patched source retain their upstream licenses. License texts are
included in [`LICENSES`](LICENSES).
