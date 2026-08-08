#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_root="${QT_ROOT_DIR:-}"
configuration="Release"
work_root=""
parallel="$(nproc)"
use_quickjs=0

usage() {
    echo "Usage: $0 [--qt-root PATH] [--configuration Debug|Release] [--work-root PATH] [--parallel N] [--use-quickjs]"
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
        --work-root)
            work_root="$2"
            shift 2
            ;;
        --parallel)
            parallel="$2"
            shift 2
            ;;
        --use-quickjs)
            use_quickjs=1
            shift 1
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

if [[ "$configuration" != "Debug" && "$configuration" != "Release" ]]; then
    echo "Configuration must be Debug or Release." >&2
    exit 2
fi
if [[ -z "$qt_root" ]]; then
    echo "Specify --qt-root or set QT_ROOT_DIR." >&2
    exit 2
fi

qt_root="$(cd "$qt_root" && pwd)"
work_root="${work_root:-$repo_root/.work/linux/$configuration}"
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

for command in cmake ninja pwsh git; do
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
install_dir="$work_root/install"
smoke_build_dir="$work_root/smoke-build"

# Clean any dirty overrides from previous builds to allow apply-patches to run cleanly
if [[ -d "$source_dir/.git" ]]; then
    echo "Resetting dirty files in build source directory..."
    git -C "$source_dir" reset --hard
    git -C "$source_dir" clean -fd
fi

echo "Building QtScript package version $qt_version for Qt at $qt_root"
pwsh -NoProfile -File "$repo_root/scripts/apply-patches.ps1" -SourceDir "$source_dir"

if [[ $use_quickjs -eq 1 ]]; then
    echo "Applying QuickJS migration overrides..."
    mkdir -p "$source_dir/src/3rdparty/quickjs"
    # Copy only *.c and *.h files to avoid shadowing standard C++ headers like <version> on case-insensitive systems
    find "$repo_root/quickjs_migration/3rdparty/quickjs" -maxdepth 1 -type f \( -name "*.c" -o -name "*.h" \) -exec cp -v {} "$source_dir/src/3rdparty/quickjs/" \;
    cp -v "$repo_root"/quickjs_migration/qscriptengine.h "$source_dir/src/script/api/"
    cp -v "$repo_root"/quickjs_migration/qscriptengine.cpp "$source_dir/src/script/api/"
    cp -v "$repo_root"/quickjs_migration/qscriptvalue.h "$source_dir/src/script/api/"
    cp -v "$repo_root"/quickjs_migration/qscriptvalue_p.h "$source_dir/src/script/api/"
    cp -v "$repo_root"/quickjs_migration/qscriptvalue.cpp "$source_dir/src/script/api/"
    cp -v "$repo_root"/quickjs_migration/qregexp.h "$source_dir/src/script/api/"
    cp -v "$repo_root"/quickjs_migration/qregexp.cpp "$source_dir/src/script/api/"
    cp -v "$repo_root"/quickjs_migration/qobject_bridge.cpp "$source_dir/src/script/bridge/"
    cp -v "$repo_root"/quickjs_migration/CMakeLists.txt "$source_dir/src/script/CMakeLists.txt"

    # Patch designated initializers in quickjs.h and cutils.h using python helper
    python3 "$repo_root/scripts/patch-quickjs-headers.py" "$source_dir/src/3rdparty/quickjs/quickjs.h" "$source_dir/src/3rdparty/quickjs/cutils.h"

    # Remove any files named VERSION or version to avoid shadowing standard C++ <version> header
    rm -f "$source_dir/src/3rdparty/quickjs/VERSION" "$source_dir/src/3rdparty/quickjs/version"
fi

"$qt_cmake" \
    -S "$source_dir" \
    -B "$build_dir" \
    -G Ninja \
    "-DCMAKE_BUILD_TYPE=$configuration" \
    "-DCMAKE_INSTALL_PREFIX=$install_dir" \
    "-DQT_REPO_MODULE_VERSION=$qt_version" \
    -DQT_BUILD_TESTS=OFF \
    -DQT_BUILD_EXAMPLES=OFF

cmake --build "$build_dir" --parallel "$parallel"
cmake --install "$build_dir"

for forbidden_path in "$source_dir" "$build_dir"; do
    if find "$install_dir" -type f \( -name '*.cmake' -o -name '*.pri' -o -name '*.prl' \) \
        -exec grep -I -F -l "$forbidden_path" {} + | grep -q .; then
        echo "Installed metadata contains path: $forbidden_path" >&2
        exit 1
    fi
done

if [[ $use_quickjs -eq 0 ]]; then
    if find "$install_dir" -type f \( -name '*.cmake' -o -name '*.pri' -o -name '*.prl' \) \
        -exec grep -I -E -l 'Core5Compat|Qt5Compat' {} + | grep -q .; then
        echo "Installed metadata contains a Core5Compat or Qt5Compat dependency." >&2
        exit 1
    fi
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

if [[ $use_quickjs -eq 0 ]]; then
    if ldd "$script_library" | grep -E 'Core5Compat|Qt5Compat'; then
        echo "QtScript links to Core5Compat or Qt5Compat." >&2
        exit 1
    fi
fi

echo "QtScript $configuration install: $install_dir"
