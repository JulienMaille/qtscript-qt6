#!/usr/bin/env bash
# Builds and installs QtScript for Qt 6 on macOS (Apple Silicon, universal
# frameworks) into an isolated prefix, then runs the external smoke consumer
# against the install.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_root="${QT_ROOT_DIR:-}"
configuration="Release"
work_root=""
install_prefix=""
parallel="$(sysctl -n hw.logicalcpu)"
architectures="arm64;x86_64"
include_ported_tests=0

usage() {
    echo "Usage: $0 [--qt-root PATH] [--configuration Debug|Release] [--work-root PATH] [--install-prefix PATH] [--parallel N] [--architectures LIST] [--include-ported-tests]"
}

while (($#)); do
    case "$1" in
        --qt-root) qt_root="$2"; shift 2 ;;
        --configuration) configuration="$2"; shift 2 ;;
        --work-root) work_root="$2"; shift 2 ;;
        --install-prefix) install_prefix="$2"; shift 2 ;;
        --parallel) parallel="$2"; shift 2 ;;
        --architectures) architectures="$2"; shift 2 ;;
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
    command -v "$command" >/dev/null ||
        { echo "$command was not found on PATH." >&2; exit 1; }
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

# The frameworks must cover every architecture Qt itself ships, or they are
# unusable from binaries built for the other architecture.
qt_core="$qt_root/lib/QtCore.framework/Versions/A/QtCore"
[[ -f "$qt_core" ]] || { echo "QtCore framework was not found under $qt_root." >&2; exit 1; }
for qt_architecture in $(lipo -archs "$qt_core"); do
    if [[ ";$architectures;" != *";$qt_architecture;"* ]]; then
        echo "Architectures are missing Qt's $qt_architecture: $architectures" >&2
        exit 1
    fi
done

source_dir="$work_root/src"
build_dir="$work_root/build"
if [[ "$include_ported_tests" -eq 1 ]]; then
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir" --include-ported-tests --include-macos
else
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir" --include-macos
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
    "-DCMAKE_OSX_ARCHITECTURES=$architectures" \
    "-DCMAKE_INSTALL_PREFIX=$install_prefix" \
    "$tests_option" -DQT_BUILD_EXAMPLES=OFF \
    "${apple_check_args[@]}" \
    -DWARNINGS_ARE_ERRORS=OFF -DQT_REPO_NOT_WARNINGS_CLEAN=ON
cmake --build "$build_dir" --parallel "$parallel"
cmake --install "$build_dir"

script_binary="$install_prefix/lib/QtScript.framework/Versions/A/QtScript"
scripttools_binary="$install_prefix/lib/QtScriptTools.framework/Versions/A/QtScriptTools"
[[ -f "$script_binary" ]] || { echo "QtScript framework was not installed." >&2; exit 1; }
[[ -f "$scripttools_binary" ]] || { echo "QtScriptTools framework was not installed." >&2; exit 1; }
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