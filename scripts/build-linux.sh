#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_root="${QT_ROOT_DIR:-}"
configuration="Release"
parallel="$(nproc)"

usage() {
    echo "Usage: $0 [--qt-root PATH] [--configuration Debug|Release] [--parallel N]"
}

while (($#)); do
    case "$1" in
        --qt-root)
            qt_root="$2"
            shift 2
            ;;
        --configuration)
            configuration="$2"
            shift 2
            ;;
        --parallel)
            parallel="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$configuration" != "Debug" && "$configuration" != "Release" ]]; then
    echo "Configuration must be Debug or Release." >&2
    exit 2
fi
if [[ -z "$qt_root" ]]; then
    echo "Specify --qt-root or set QT_ROOT_DIR." >&2
    exit 2
fi
if [[ ! -d "$qt_root" ]]; then
    echo "Qt root directory does not exist: $qt_root" >&2
    exit 2
fi
if [[ ! "$parallel" =~ ^[0-9]+$ || "$parallel" -lt 1 ]]; then
    echo "Parallel must be a positive integer." >&2
    exit 2
fi

qt_root="$(cd "$qt_root" && pwd)"
work_root="$repo_root/.work/linux/$configuration"
mkdir -p "$work_root"
work_root="$(cd "$work_root" && pwd)"

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

for command in cmake ninja; do
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

source_dir="$work_root/src"
build_dir="$work_root/build"
smoke_build_dir="$work_root/smoke-build"

echo "Building QtScript package version $qt_version for Qt at $qt_root"
bash "$repo_root/scripts/apply-patches.sh" "$source_dir"

"$qt_cmake" \
    -S "$source_dir" \
    -B "$build_dir" \
    -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_INSTALL_PREFIX=$qt_root" \
    "-DQT_REPO_MODULE_VERSION=$qt_version" \
    -DQT_BUILD_TESTS=OFF \
    -DQT_BUILD_EXAMPLES=OFF

cmake --build "$build_dir" --parallel "$parallel"
cmake --install "$build_dir"

for forbidden_path in "$source_dir" "$build_dir"; do
    if find "$qt_root/lib/cmake/Qt6Script" "$qt_root/mkspecs/modules/qt_lib_script.pri" \
        -type f \( -name '*.cmake' -o -name '*.pri' -o -name '*.prl' \) \
        -exec grep -I -F -l "$forbidden_path" {} + 2>/dev/null | grep -q .; then
        echo "Installed metadata contains path: $forbidden_path" >&2
        exit 1
    fi
done

cmake \
    -S "$repo_root/tests/smoke" \
    -B "$smoke_build_dir" \
    -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_PREFIX_PATH=$qt_root"

cmake --build "$smoke_build_dir" --parallel "$parallel"
LD_LIBRARY_PATH="$qt_root/lib:${LD_LIBRARY_PATH:-}" \
    ctest --test-dir "$smoke_build_dir" --output-on-failure

echo "QtScript $configuration installed into $qt_root"
