#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_root="${QT_ROOT_DIR:-}"
configuration="Release"
work_root=""
parallel="$(nproc)"
include_ported_tests=0

while (($#)); do
    case "$1" in
        --qt-root) qt_root="$2"; shift 2 ;;
        --configuration) configuration="$2"; shift 2 ;;
        --work-root) work_root="$2"; shift 2 ;;
        --parallel) parallel="$2"; shift 2 ;;
        --include-ported-tests) include_ported_tests=1; shift ;;
        -h|--help)
            echo "Usage: $0 [--qt-root PATH] [--configuration Debug|Release] [--work-root PATH] [--parallel N] [--include-ported-tests]"
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
LD_LIBRARY_PATH="$qt_root/lib:${LD_LIBRARY_PATH:-}" \
    ctest --test-dir "$smoke_dir" --output-on-failure

echo "QtScript $configuration installed into $qt_root"
