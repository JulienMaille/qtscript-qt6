# QtScript for Qt 6

[![Windows Qt 6.9](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows.yml)
[![Windows Qt 6.10](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-qt610.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/windows-qt610.yml)
[![Linux Qt 6.9](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux.yml/badge.svg)](https://github.com/JulienMaille/qtscript-qt6/actions/workflows/linux.yml)

This repository provides the patches needed to build the QtScript core module
with Qt 6 on Windows x64/MSVC 2022 and Linux x64/GCC. Qt 6.9.2 is the baseline;
CI also tests Qt 6.10.2 on Windows.

QtScript source is not vendored. The build script clones KDE's QtScript
5.15.19 revision and applies the ordered files in [`patches`](patches).

## Status

- Builds Debug and Release variants of `Qt6::Script`.
- Installs DLLs, import libraries, headers and CMake package files into a
  separate QtScript prefix; the Qt installation is not modified.
- Preserves the public `QScript*` source API. Qt 5 binary compatibility is not
  supported.
- Does not depend on Qt5Compat or Core5Compat.
- Tests evaluation, calls, exceptions, `QVariant`, `QRegExp`, QObject exposure,
  enum conversion and signals.

Legacy `QRegExp` signatures use `QtScript/QRegExp`, implemented with Qt 6
`QRegularExpression`. Regular-expression, wildcard, fixed-string, capture and
replacement behavior is supported. Less common Qt 5 `QRegExp` behavior may
differ and should be covered by application-specific tests.

See [`PORTING.md`](PORTING.md) for the patch inventory and
[`docs/VALIDATION.md`](docs/VALIDATION.md) for test results.

## Requirements

- Git and CMake 3.22 or newer.
- Windows: Visual Studio 2022 with the MSVC x64 C++ toolchain.
- Linux: GCC, Ninja, Bash and PowerShell.
- A Qt 6.9.2 or newer installation with private module build tooling
  (`qt-cmake-private`).

## Build (Original JSC Backend)

```powershell
.\scripts\build-windows.ps1 `
  -QtRoot C:\Qt\6.9.2\msvc2022_64 `
  -Configuration Release
```

The script clones and patches QtScript, builds it, installs it to
`.work\Release\install`, and runs an external CMake smoke test. Use
`-Configuration Debug` for Debug.

To prepare source without building:

```powershell
.\scripts\apply-patches.ps1 -SourceDir D:\work\qtscript-qt6-src
```

Add `-IncludePortedTests` to apply the optional upstream test adaptations from
`patches/optional/tests`.

On Linux:

```bash
./scripts/build-linux.sh --qt-root "$HOME/Qt/6.9.2/gcc_64" --configuration Release
```

## QuickJS Migration (Modern Standalone Option)

A fully-functional, compiled, and smoke-tested C++ bridging layer that replaces legacy 2011 JavaScriptCore with **QuickJS (stable release 2024-01-13)** is implemented under `quickjs_migration/`.

### **Advantages of the QuickJS Backend:**
- **Fully Standalone:** The QuickJS C engine is statically compiled directly into `QtScript.dll` / `libQt6Script.so`. No extra QuickJS DLL or dependency distribution is required.
- **Massive Size Reduction:** The QuickJS-powered binary is only **1.1 MB** (compared to **2.5 MB** of the original JavaScriptCore version)—a **56% reduction** in binary footprint.
- **Improved Syntax and Standards:** Native support for modern ECMAScript standards (ES2020) and enhanced security compared to the legacy 2011 JSC snapshot.
- **Clean Decoupling:** Uses the Pimpl idiom to completely decouple private engine headers from public headers, ensuring consumers only need standard QtScript headers to compile.

### **Build QuickJS Backend on Linux:**

```bash
./scripts/build-quickjs.sh
```

---

## Use the isolated install prefix

The QtScript install prefix is an add-on package next to the existing Qt
installation. It does not copy files into Qt.

```powershell
$qtScriptPrefix = (Resolve-Path .\.work\Release\install).Path -replace '\\', '/'

cmake -S . -B build `
  "-DCMAKE_PREFIX_PATH=$qtScriptPrefix;C:/Qt/6.9.2/msvc2022_64" `
  "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=$qtScriptPrefix"
```

Use the normal Qt target:

```cmake
find_package(Qt6 6.9.2 REQUIRED COMPONENTS Script)
target_link_libraries(my_target PRIVATE Qt6::Script)
```

At runtime on Windows, place `<QtScript prefix>/bin` and `<Qt prefix>/bin` on
`PATH`. On Linux, add their `lib` directories to the dynamic linker search
path. CMake consumption is tested. qmake consumption is not currently
supported.

## Security

QtScript embeds a legacy 2011 JavaScriptCore snapshot. It is not a security
sandbox; run only trusted scripts. See [`SECURITY.md`](SECURITY.md).

## License

QtScript and patched source retain their upstream licenses. License texts are
included in [`LICENSES`](LICENSES).
