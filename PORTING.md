# Porting notes

## Reproducible baseline

- Repository: `https://invent.kde.org/qt/qt/qtscript.git`
- Revision: `bcd7cae6215df8f1c8b45a338f3327da51edeaff`
- KDE branch/tag context: QtScript 5.15.19
- Baseline tree: `9f515614bafcf1b8bf6741e77e0cded7ebe6b5f5`
- Default core-port tree: `63bf6ef09faee052851a504425d00f3ea105bbfd`
- Core plus test-port tree: `23a582cc6c55e9bf0be3f3513e9c1a76a56d89e1`

The repository contains patches rather than an upstream source snapshot.
[`scripts/apply-patches.ps1`](scripts/apply-patches.ps1) clones the KDE origin,
checks out the pinned baseline, applies the requested layers in lexical order
with `git am`, and verifies the tree after every layer. Commit identifiers can
vary because `git am` records a new committer timestamp.

## Default patch series

1. `0001-Add-minimal-Qt-6-CMake-core-build.patch` adds the CMake
   module definition and explicit source/header manifests for the interpreter
   build. Inactive JIT, ARM, Symbian, and other unused translation units are
   excluded.
2. `0002-Port-JavaScriptCore-subset-to-C-17.patch` contains only the
   C++17/MSVC adaptations exercised by that source manifest.
3. `0003-Adapt-QtScript-API-and-metatypes-to-Qt-6.patch` handles Qt 6
   API, container, enum/metatype, atomic, and date/time differences in the
   public API and engine.
4. `0004-Adapt-QObject-bridge-to-Qt-6.patch` contains the QObject,
   Qt 6 moc-format metaobject, method invocation, property, and signal bridge
   changes.
5. `0005-Replace-removed-QBoolBlocker-helper.patch` replaces the removed
   private Qt helper with the public `QScopedValueRollback<bool>` equivalent,
   keeping the core compatible with Qt 6.10 and newer.
6. `0006-Remove-Core5Compat-dependency.patch` supplies the legacy public
   `QRegExp` API from QtScript using `QRegularExpression`, removing the module's
   build, link, and package-metadata dependency on Qt Core5Compat and obsolete
   Qt 5 regexp feature guards.
7. `0007-Add-Linux-core-build-support.patch` selects the platform-specific
   JavaScriptCore stack allocator and keeps Windows-only definitions and
   libraries out of Linux builds.

## Optional layers

- `patches/optional/tests` updates selected upstream tests and is applied with
  `-IncludePortedTests`. The normal module build keeps `QT_BUILD_TESTS=OFF`.

The optional test layer is not required to compile or smoke-test the core
module.

## Build scope

The Qt 6 CMake entry point currently builds the core `Script` module only. It
exports `Qt6::Script`, public/private headers, and CMake package metadata.
Examples, documentation, qmake integration, x86, platforms other than Windows
and Linux, and ScriptTools are not part of the current acceptance scope.

The source baseline defaults to package version 6.9.2, while
`build-windows.ps1` reads the selected installation with `qtpaths` and supplies
its actual version to CMake. This keeps the generated add-on metadata aligned
when the compatibility workflow builds against a newer Qt release.

## Updating the patch set

Prepare a fresh source tree, make focused commits on top of the pinned baseline,
and regenerate the series with:

```powershell
git format-patch `
  --output-directory D:\Dev\qtscript-qt6\patches `
  bcd7cae6215df8f1c8b45a338f3327da51edeaff..HEAD
```

Regenerate each logical layer from its direct parent, update the corresponding
expected tree in `apply-patches.ps1`, then run both Debug and Release builds
before publishing the change.
