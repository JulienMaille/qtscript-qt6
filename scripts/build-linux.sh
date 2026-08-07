#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_root="${QT_ROOT_DIR:-}"
configuration="Release"
work_root=""
parallel="$(nproc)"
include_ported_tests=0
run_ported_tests=0

usage() {
    echo "Usage: $0 [--qt-root PATH] [--configuration Debug|Release] [--work-root PATH] [--parallel N] [--include-ported-tests] [--run-ported-tests]"
}

while (($#)); do
    case "$1" in
        --qt-root)
            if (($# < 2)); then
                echo "--qt-root requires a path." >&2
                exit 2
            fi
            qt_root="$2"
            shift 2
            ;;
        --configuration)
            if (($# < 2)); then
                echo "--configuration requires Debug or Release." >&2
                exit 2
            fi
            configuration="$2"
            shift 2
            ;;
        --work-root)
            if (($# < 2)); then
                echo "--work-root requires a path." >&2
                exit 2
            fi
            work_root="$2"
            shift 2
            ;;
        --parallel)
            if (($# < 2)); then
                echo "--parallel requires a positive integer." >&2
                exit 2
            fi
            parallel="$2"
            shift 2
            ;;
        --include-ported-tests)
            include_ported_tests=1
            shift
            ;;
        --run-ported-tests)
            run_ported_tests=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

# Running the ported suites implies building them.
if [[ "$run_ported_tests" -eq 1 ]]; then
    include_ported_tests=1
fi

if [[ "$configuration" != "Debug" && "$configuration" != "Release" ]]; then
    echo "Configuration must be Debug or Release." >&2
    exit 2
fi
if [[ -z "$qt_root" ]]; then
    echo "Specify --qt-root or set QT_ROOT_DIR." >&2
    exit 2
fi

qt_root="$(cd "$qt_root" && pwd)"

qt_cmake=""
for candidate in "$qt_root/bin/qt-cmake-private" "$qt_root/libexec/qt-cmake-private"; do
    if [[ -x "$candidate" ]]; then
        qt_cmake="$candidate"
        break
    fi
done
if [[ -z "$qt_cmake" ]]; then
    echo "Qt private module build helper was not found under $qt_root." >&2
    exit 1
fi

qtpaths=""
for candidate in "$qt_root/bin/qtpaths6" "$qt_root/bin/qtpaths"; do
    if [[ -x "$candidate" ]]; then
        qtpaths="$candidate"
        break
    fi
done
if [[ -z "$qtpaths" ]]; then
    echo "qtpaths was not found under $qt_root/bin." >&2
    exit 1
fi

for command in cmake ninja git ldd; do
    if ! command -v "$command" >/dev/null; then
        echo "$command was not found on PATH." >&2
        exit 1
    fi
done

qt_version="$($qtpaths --qt-version)"
if [[ ! "$qt_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Unable to determine the Qt version from $qtpaths." >&2
    exit 1
fi

if [[ -z "$work_root" ]]; then
    work_root="$repo_root/.work/linux/$qt_version/$configuration"
fi
mkdir -p "$work_root"
work_root="$(cd "$work_root" && pwd)"

source_dir="$work_root/src"
build_dir="$work_root/build"
install_dir="$work_root/install"
smoke_build_dir="$work_root/smoke-build"

echo "Building QtScript package version $qt_version for Qt at $qt_root"
if [[ "$include_ported_tests" -eq 1 ]]; then
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir" --include-ported-tests
else
    bash "$repo_root/scripts/apply-patches.sh" "$source_dir"
fi

tests_option="-DQT_BUILD_TESTS=OFF"
if [[ "$include_ported_tests" -eq 1 ]]; then
    tests_option="-DQT_BUILD_TESTS=ON"
fi

"$qt_cmake" \
    -S "$source_dir" \
    -B "$build_dir" \
    -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_INSTALL_PREFIX=$qt_root" \
    "$tests_option" \
    -DQT_BUILD_EXAMPLES=OFF

cmake --build "$build_dir" --parallel "$parallel"
cmake --install "$build_dir"

if [[ "$run_ported_tests" -eq 1 ]]; then
    echo "Running ported upstream test suites (ctest, $configuration)..."
    QT_QPA_PLATFORM=offscreen \
        ctest --test-dir "$build_dir" --parallel "$parallel" --output-on-failure
fi

metadata_files=()
while IFS= read -r -d '' metadata_file; do
    metadata_files+=("$metadata_file")
done < <(
    find "$qt_root/lib/cmake/Qt6Script" "$qt_root/mkspecs/modules/qt_lib_script.pri" \
        -type f \( -name '*.cmake' -o -name '*.pri' -o -name '*.prl' \) \
        -print0 2>/dev/null
)
if ((${#metadata_files[@]} == 0)); then
    echo "Installed QtScript metadata was not found under $qt_root." >&2
    exit 1
fi

for forbidden_path in "$source_dir" "$build_dir"; do
    if grep -I -F -l "$forbidden_path" "${metadata_files[@]}" >/dev/null; then
        echo "Installed metadata contains path: $forbidden_path" >&2
        exit 1
    fi
done
if grep -I -E -l 'Core5Compat|Qt5Compat' "${metadata_files[@]}" >/dev/null; then
    echo "Installed metadata contains a Core5Compat or Qt5Compat dependency." >&2
    exit 1
fi

script_library="$(find "$qt_root/lib" -maxdepth 1 \( -type f -o -type l \) \
    -name 'libQt6Script.so*' -print -quit 2>/dev/null || true)"
if [[ -z "$script_library" ]]; then
    echo "Installed QtScript shared library was not found under $qt_root/lib." >&2
    exit 1
fi
if ldd "$script_library" 2>&1 | grep -E 'Core5Compat|Qt5Compat' >/dev/null; then
    echo "QtScript links to Core5Compat or Qt5Compat." >&2
    exit 1
fi

if find "$install_dir" -type f \( -name '*.cmake' -o -name '*.pri' -o -name '*.prl' \) \
    -exec grep -I -E -l 'Core5Compat|Qt5Compat' {} + | grep -q .; then
    echo "Installed metadata contains a Core5Compat or Qt5Compat dependency." >&2
    exit 1
fi

cmake \
    -S "$repo_root/tests/smoke" \
    -B "$smoke_build_dir" \
    -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_PREFIX_PATH=$install_dir;$qt_root" \
    "-DQT_ADDITIONAL_PACKAGES_PREFIX_PATH=$install_dir"

cmake --build "$smoke_build_dir" --parallel "$parallel"
LD_LIBRARY_PATH="$install_dir/lib:$qt_root/lib:${LD_LIBRARY_PATH:-}" \
    ctest --test-dir "$smoke_build_dir" --output-on-failure

script_library="$(find "$install_dir/lib" -maxdepth 1 -type f -name 'libQt6Script.so*' | head -n 1)"
if [[ -z "$script_library" ]]; then
    echo "Installed QtScript shared library was not found." >&2
    exit 1
fi
if ldd "$script_library" | grep -E 'Core5Compat|Qt5Compat'; then
    echo "QtScript links to Core5Compat or Qt5Compat." >&2
    exit 1
fi

echo "QtScript $configuration install: $install_dir"
