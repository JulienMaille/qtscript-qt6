#!/usr/bin/env bash
# Builds and installs the QuickJS-backend QtScript for Qt 6 on macOS
# (Apple Silicon, universal frameworks) into an isolated prefix, then runs
# the external smoke consumer against the install.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected_quickjs_commit="954dc53628e36891f93c359aa60895c2ae3dac6b"
qt_root="${QT_ROOT_DIR:-}"
configuration="Release"
work_root=""
install_prefix=""
parallel="$(sysctl -n hw.logicalcpu)"
include_ported_tests=0
quickjs_source=""
quickjs_library=""

usage() {
    echo "Usage: $0 [--qt-root PATH] [--configuration Debug|Release] [--work-root PATH] [--install-prefix PATH] [--parallel N] [--quickjs-source PATH] [--quickjs-library PATH] [--include-ported-tests]"
}

while (($#)); do
    case "$1" in
        --qt-root) qt_root="$2"; shift 2 ;;
        --configuration) configuration="$2"; shift 2 ;;
        --work-root) work_root="$2"; shift 2 ;;
        --install-prefix) install_prefix="$2"; shift 2 ;;
        --parallel) parallel="$2"; shift 2 ;;
        --quickjs-source) quickjs_source="$2"; shift 2 ;;
        --quickjs-library) quickjs_library="$2"; shift 2 ;;
        --include-ported-tests) include_ported_tests=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

[[ "$configuration" == Debug || "$configuration" == Release ]] ||
    { echo "Configuration must be Debug or Release." >&2; exit 2; }
[[ -n "$qt_root" && -d "$qt_root" ]] ||
    { echo "Specify --qt-root or set QT_ROOT_DIR." >&2; exit 1; }
[[ "$parallel" =~ ^[0-9]+$ && "$parallel" -ge 1 ]] ||
    { echo "Parallel must be a positive integer." >&2; exit 2; }
for command in cmake ninja git lipo otool shasum; do
    command -v "$command" >/dev/null || { echo "Missing required command: $command" >&2; exit 1; }
done

qt_root="$(cd "$qt_root" && pwd)"
qt_key="$(printf '%s' "$qt_root" | shasum -a 256)"
qt_key="${qt_key:0:12}"
qt_cmake=""
for candidate in "$qt_root/bin/qt-cmake-private" "$qt_root/libexec/qt-cmake-private"; do
    [[ -x "$candidate" ]] && { qt_cmake="$candidate"; break; }
done
[[ -n "$qt_cmake" ]] ||
    { echo "Qt private module build helper was not found under $qt_root." >&2; exit 1; }

[[ -n "$work_root" ]] || work_root="$repo_root/.work/$qt_key/$configuration"
[[ -n "$install_prefix" ]] || install_prefix="$work_root/install"
[[ -n "$quickjs_source" ]] || quickjs_source="$repo_root/third_party/quickjs-ng"
if [[ -z "$quickjs_library" ]]; then
    quickjs_build="$repo_root/.work/quickjs-ng/$configuration/build"
    quickjs_library="$quickjs_build/libqjs.a"
fi

[[ -f "$quickjs_source/quickjs.h" ]] ||
    { echo "QuickJS-NG headers were not found: $quickjs_source" >&2; exit 1; }
[[ -f "$quickjs_library" ]] ||
    { echo "QuickJS-NG static library was not found: $quickjs_library" >&2; exit 1; }
quickjs_source="$(cd "$quickjs_source" && pwd)"
quickjs_library="$(cd "$(dirname "$quickjs_library")" && pwd)/$(basename "$quickjs_library")"
actual_quickjs_commit="$(git -C "$quickjs_source" rev-parse HEAD)"
[[ "$actual_quickjs_commit" == "$expected_quickjs_commit" ]] ||
    { echo "QuickJS-NG revision mismatch: expected $expected_quickjs_commit, got $actual_quickjs_commit." >&2; exit 1; }
quickjs_marker="$(dirname "$quickjs_library")/.qtscript-quickjs-build"
if [[ ! -f "$quickjs_marker" ]] ||
   ! grep -Fxq "commit=$expected_quickjs_commit" "$quickjs_marker" ||
   ! grep -Fxq "configuration=$configuration" "$quickjs_marker"; then
    echo "QuickJS-NG build metadata does not match the pinned commit and $configuration library." >&2
    exit 1
fi

# The QuickJS static archive must cover every architecture Qt itself ships,
# or the module is unusable from binaries built for the other architecture.
qt_core="$qt_root/lib/QtCore.framework/Versions/A/QtCore"
[[ -f "$qt_core" ]] || { echo "QtCore framework was not found under $qt_root." >&2; exit 1; }
quickjs_architectures="$(lipo -archs "$quickjs_library")"
for qt_architecture in $(lipo -archs "$qt_core"); do
    if [[ " $quickjs_architectures " != *" $qt_architecture "* ]]; then
        echo "QuickJS-NG is missing Qt's $qt_architecture architecture: $quickjs_library" >&2
        exit 1
    fi
done

source_dir="$work_root/src"
build_dir="$work_root/build"
if [[ "$include_ported_tests" -eq 1 ]]; then
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir" --include-ported-tests
else
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir"
fi

tests_option=-DQT_BUILD_TESTS=OFF
[[ "$include_ported_tests" -eq 1 ]] && tests_option=-DQT_BUILD_TESTS=ON

apple_check_args=(-DQT_FORCE_WARN_APPLE_SDK_AND_XCODE_CHECK=ON)
if ! command -v xcodebuild >/dev/null || ! xcodebuild -version >/dev/null 2>&1; then
    # Qt 6.11 otherwise rejects Command Line Tools-only hosts before compiling.
    # GitHub's macOS runners have full Xcode and retain Qt's version check.
    apple_check_args+=(-DQT_NO_XCODE_MIN_VERSION_CHECK=ON)
fi

"$qt_cmake" -S "$source_dir" -B "$build_dir" -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_INSTALL_PREFIX=$install_prefix" \
    "-DQTSCRIPT_QUICKJS_INCLUDE_DIR=$quickjs_source" \
    "-DQTSCRIPT_QUICKJS_LIBRARY=$quickjs_library" \
    "$tests_option" -DQT_BUILD_EXAMPLES=OFF \
    "${apple_check_args[@]}" \
    -DWARNINGS_ARE_ERRORS=OFF -DQT_REPO_NOT_WARNINGS_CLEAN=ON
cmake --build "$build_dir" --parallel "$parallel"
cmake --install "$build_dir"

script_binary="$install_prefix/lib/QtScript.framework/Versions/A/QtScript"
scripttools_binary="$install_prefix/lib/QtScriptTools.framework/Versions/A/QtScriptTools"
[[ -f "$script_binary" ]] || { echo "QtScript framework was not installed." >&2; exit 1; }
[[ -f "$scripttools_binary" ]] || { echo "QtScriptTools framework was not installed." >&2; exit 1; }
# Universality is intentionally implicit: no -DCMAKE_OSX_ARCHITECTURES is
# passed because qt-cmake-private already defaults to Qt's own architectures.
# The installed frameworks must still cover every architecture Qt itself
# ships, or they are unusable from binaries built for the other architecture.
for qt_architecture in $(lipo -archs "$qt_core"); do
    for binary in "$script_binary" "$scripttools_binary"; do
        if [[ " $(lipo -archs "$binary") " != *" $qt_architecture "* ]]; then
            echo "$binary is missing Qt's $qt_architecture architecture." >&2
            exit 1
        fi
    done
done
# The one claim worth asserting: the module does not link Core5Compat.
if otool -L "$script_binary" | grep -E 'Core5Compat|Qt5Compat' >/dev/null; then
    echo "QtScript links to Core5Compat or Qt5Compat." >&2
    exit 1
fi

# Out-of-tree smoke: build and run the external consumer against the install.
smoke_dir="$work_root/smoke-build"
cmake -S "$repo_root/tests/smoke" -B "$smoke_dir" -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_PREFIX_PATH=$qt_root" \
    "-DQt6Script_DIR=$install_prefix/lib/cmake/Qt6Script" \
    "-DQt6ScriptTools_DIR=$install_prefix/lib/cmake/Qt6ScriptTools"
cmake --build "$smoke_dir" --parallel "$parallel"
QT_QPA_PLATFORM=offscreen \
DYLD_FRAMEWORK_PATH="$install_prefix/lib:${DYLD_FRAMEWORK_PATH:-}" \
    ctest --test-dir "$smoke_dir" --output-on-failure

echo "QtScript $configuration installed into $install_prefix"
