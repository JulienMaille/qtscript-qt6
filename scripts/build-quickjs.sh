#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_root="/home/jules/Qt/6.9.2/gcc_64"
work_root="$repo_root/.work/linux/Release"

# 1. Run baseline prep if not already there
if [ ! -d "$work_root/src" ]; then
    echo "Running build-linux.sh once to prepare source..."
    ./scripts/build-linux.sh --qt-root "$qt_root" --configuration Release --parallel 4
fi

echo "Copying QuickJS migration files into checked out KDE repository..."

# Copy QuickJS engine source
mkdir -p "$work_root/src/src/3rdparty/quickjs"
cp -v "$repo_root"/quickjs_migration/3rdparty/quickjs/* "$work_root/src/src/3rdparty/quickjs/"

# Copy QScript API classes
cp -v "$repo_root"/quickjs_migration/qscriptengine.h "$work_root/src/src/script/api/"
cp -v "$repo_root"/quickjs_migration/qscriptengine.cpp "$work_root/src/src/script/api/"
cp -v "$repo_root"/quickjs_migration/qscriptvalue.h "$work_root/src/src/script/api/"
cp -v "$repo_root"/quickjs_migration/qscriptvalue_p.h "$work_root/src/src/script/api/"
cp -v "$repo_root"/quickjs_migration/qscriptvalue.cpp "$work_root/src/src/script/api/"
cp -v "$repo_root"/quickjs_migration/qregexp.h "$work_root/src/src/script/api/"
cp -v "$repo_root"/quickjs_migration/qregexp.cpp "$work_root/src/src/script/api/"

# Copy QObject bridge class
cp -v "$repo_root"/quickjs_migration/qobject_bridge.cpp "$work_root/src/src/script/bridge/"

# Copy CMakeLists.txt
cp -v "$repo_root"/quickjs_migration/CMakeLists.txt "$work_root/src/src/script/CMakeLists.txt"

echo "Running build on the QuickJS-powered QtScript library..."

qt_cmake="$qt_root/libexec/qt-cmake-private"
build_dir="$work_root/build"
install_dir="$work_root/install"
smoke_build_dir="$work_root/smoke-build"

# Re-configure and rebuild
cd "$build_dir"
cmake .
cmake --build . --parallel 4
cmake --install .

# Re-configure smoke test
cd "$smoke_build_dir"
cmake .
cmake --build . --parallel 4

echo "Running the QuickJS-powered Smoke Test!"
LD_LIBRARY_PATH="$install_dir/lib:$qt_root/lib" ./qtscript_smoke
