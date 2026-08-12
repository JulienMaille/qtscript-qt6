#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
expected_quickjs_commit="954dc53628e36891f93c359aa60895c2ae3dac6b"
qt_root="${QT_ROOT_DIR:-}"
configuration="Release"
work_root=""
parallel="$(nproc)"
include_ported_tests=0
quickjs_source=""
quickjs_library=""

while (($#)); do
    case "$1" in
        --qt-root) qt_root="$2"; shift 2 ;;
        --configuration) configuration="$2"; shift 2 ;;
        --work-root) work_root="$2"; shift 2 ;;
        --parallel) parallel="$2"; shift 2 ;;
        --quickjs-source) quickjs_source="$2"; shift 2 ;;
        --quickjs-library) quickjs_library="$2"; shift 2 ;;
        --include-ported-tests) include_ported_tests=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--qt-root PATH] [--configuration Debug|Release] [--work-root PATH] [--parallel N] [--quickjs-source PATH] [--quickjs-library PATH] [--include-ported-tests]"
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ "$configuration" == Debug || "$configuration" == Release ]] ||
    { echo "Configuration must be Debug or Release." >&2; exit 2; }
[[ -n "$qt_root" && -d "$qt_root" ]] ||
    { echo "Specify --qt-root or set QT_ROOT_DIR." >&2; exit 1; }
[[ "$parallel" =~ ^[0-9]+$ && "$parallel" -ge 1 ]] ||
    { echo "Parallel must be a positive integer." >&2; exit 2; }
for command in cmake ninja git ldd sha256sum; do
    command -v "$command" >/dev/null ||
        { echo "$command was not found on PATH." >&2; exit 1; }
done

qt_root="$(cd "$qt_root" && pwd)"
qt_key="$(printf '%s' "$qt_root" | sha256sum)"
qt_key="${qt_key:0:12}"
qt_cmake=""
for candidate in "$qt_root/bin/qt-cmake-private" "$qt_root/libexec/qt-cmake-private"; do
    [[ -x "$candidate" ]] && { qt_cmake="$candidate"; break; }
done
[[ -n "$qt_cmake" ]] ||
    { echo "Qt private module build helper was not found under $qt_root." >&2; exit 1; }

[[ -n "$work_root" ]] || work_root="$repo_root/.work/$qt_key/$configuration"

if [[ -z "$quickjs_source" ]]; then
    quickjs_source="$repo_root/third_party/quickjs-ng"
fi
if [[ -z "$quickjs_library" ]]; then
    quickjs_build="$repo_root/.work/quickjs-ng/$configuration/build"
    quickjs_library="$quickjs_build/libqjs.a"
    [[ -f "$quickjs_build/$configuration/libqjs.a" ]] && quickjs_library="$quickjs_build/$configuration/libqjs.a"
fi
[[ -f "$quickjs_source/quickjs.h" ]] ||
    { echo "QuickJS-NG headers were not found: $quickjs_source. Initialize the submodule first." >&2; exit 1; }
[[ -f "$quickjs_library" ]] ||
    { echo "QuickJS-NG static library was not found: $quickjs_library. Run scripts/build-quickjs-ng.sh --configuration $configuration first." >&2; exit 1; }
quickjs_source="$(cd "$quickjs_source" && pwd)"
quickjs_library="$(cd "$(dirname "$quickjs_library")" && pwd)/$(basename "$quickjs_library")"
actual_quickjs_commit="$(git -C "$quickjs_source" rev-parse HEAD)"
[[ "$actual_quickjs_commit" == "$expected_quickjs_commit" ]] ||
    { echo "QuickJS-NG revision mismatch: expected $expected_quickjs_commit, got $actual_quickjs_commit." >&2; exit 1; }
quickjs_marker="$(dirname "$quickjs_library")/.qtscript-quickjs-build"
if [[ ! -f "$quickjs_marker" ]] ||
   ! grep -Fxq "commit=$expected_quickjs_commit" "$quickjs_marker" ||
   ! grep -Fxq "configuration=$configuration" "$quickjs_marker"; then
    echo "QuickJS-NG build metadata does not match the pinned commit and $configuration library. Rebuild it with scripts/build-quickjs-ng.sh." >&2
    exit 1
fi

source_dir="$work_root/src"
build_dir="$work_root/build"

if [[ "$include_ported_tests" -eq 1 ]]; then
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir" --include-ported-tests
else
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir"
fi

tests_option=-DQT_BUILD_TESTS=OFF
[[ "$include_ported_tests" -eq 1 ]] && tests_option=-DQT_BUILD_TESTS=ON

"$qt_cmake" -S "$source_dir" -B "$build_dir" -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_INSTALL_PREFIX=$qt_root" \
    "-DQTSCRIPT_QUICKJS_INCLUDE_DIR=$quickjs_source" \
    "-DQTSCRIPT_QUICKJS_LIBRARY=$quickjs_library" \
    "$tests_option" -DQT_BUILD_EXAMPLES=OFF
cmake --build "$build_dir" --parallel "$parallel"
cmake --install "$build_dir"

# The one claim worth asserting: the module does not link Core5Compat.
lib="$(find "$qt_root/lib" -maxdepth 1 -name 'libQt6Script.so*' -print -quit 2>/dev/null || true)"
if ldd "$lib" 2>&1 | grep -E 'Core5Compat|Qt5Compat' >/dev/null; then
    echo "QtScript links to Core5Compat or Qt5Compat." >&2
    exit 1
fi

# Out-of-tree smoke: build and run the external consumer against the install.
smoke_dir="$work_root/smoke-build"
cmake -S "$repo_root/tests/smoke" -B "$smoke_dir" -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" "-DCMAKE_PREFIX_PATH=$qt_root"
cmake --build "$smoke_dir" --parallel "$parallel"
QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH="$qt_root/lib:${LD_LIBRARY_PATH:-}" \
    ctest --test-dir "$smoke_dir" --output-on-failure

echo "QtScript $configuration installed into $qt_root"
