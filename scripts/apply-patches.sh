#!/usr/bin/env bash
# Applies the QtScript Qt 6 patch series to a clean checkout of the
# QtScript 5.15.19 release.
#
# Usage: apply-patches.sh SOURCE_DIR [--include-ported-tests]
#   SOURCE_DIR            work tree to prepare (cloned if missing)
#   --include-ported-tests  also apply patches/optional/tests

set -euo pipefail

base_branch="5.15.19"
repository="https://invent.kde.org/qt/qt/qtscript.git"

source_dir=""
include_tests=0
include_macos=0
while (($#)); do
    case "$1" in
        --include-ported-tests)
            include_tests=1
            shift
            ;;
        --include-macos)
            include_macos=1
            shift
            ;;
        -*)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 SOURCE_DIR [--include-ported-tests] [--include-macos]" >&2
            exit 2
            ;;
        *)
            if [[ -n "$source_dir" ]]; then
                echo "Unexpected argument: $1" >&2
                exit 2
            fi
            source_dir="$1"
            shift
            ;;
    esac
done

if [[ -z "$source_dir" ]]; then
    echo "Usage: $0 SOURCE_DIR [--include-ported-tests] [--include-macos]" >&2
    exit 2
fi
source_dir="$(cd "$source_dir" 2>/dev/null && pwd || echo "$source_dir")"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v git >/dev/null; then
    echo "git was not found on PATH." >&2
    exit 1
fi

if [[ ! -e "$source_dir/.git" ]]; then
    if [[ -d "$source_dir" ]]; then
        if [[ -n "$(ls -A "$source_dir")" ]]; then
            echo "SourceDir exists and is not empty: $source_dir" >&2
            exit 1
        fi
    else
        mkdir -p "$(dirname "$source_dir")"
    fi

    git clone --depth 1 --branch "$base_branch" --single-branch "$repository" "$source_dir"
fi

if [[ -d "$source_dir/.git/rebase-apply" ]]; then
    echo "SourceDir has an interrupted git am session (.git/rebase-apply). Resolve or abort it first: $source_dir" >&2
    exit 1
fi

if [[ -n "$(git -C "$source_dir" status --porcelain)" ]]; then
    echo "The QtScript source tree has uncommitted changes: $source_dir" >&2
    exit 1
fi

apply_patches() {
    local patch_dir="$1"
    shopt -s nullglob
    patches=("$patch_dir"/*.patch)
    shopt -u nullglob
    if ((${#patches[@]} == 0)); then
        echo "No patches were found in $patch_dir" >&2
        exit 1
    fi

    echo "Applying ${#patches[@]} patches from $patch_dir"
    if ! git -C "$source_dir" \
        -c 'user.name=QtScript Qt 6 patch set' \
        -c 'user.email=qtscript-qt6@local.invalid' \
        am "${patches[@]}"; then
        git -C "$source_dir" am --abort || true
        echo "Failed to apply patches from $patch_dir" >&2
        exit 1
    fi
}

if [[ ! -f "$source_dir/CMakeLists.txt" ]]; then
    cp "$repo_root/cmake/CMakeLists.txt" "$source_dir/"
    cp "$repo_root/cmake/.cmake.conf" "$source_dir/"
    cp "$repo_root/cmake/src/CMakeLists.txt" "$source_dir/src/"
    cp "$repo_root/cmake/src/script/CMakeLists.txt" "$source_dir/src/script/"
    cp "$repo_root/cmake/src/scripttools/CMakeLists.txt" "$source_dir/src/scripttools/"
    cp "$repo_root/cmake/src/scripttools/Qt6ScriptToolsMacOSHelpers.cmake" "$source_dir/src/scripttools/"
    apply_patches "$repo_root/patches"
fi

if ((include_tests)) && [[ ! -f "$source_dir/tests/CMakeLists.txt" ]]; then
    apply_patches "$repo_root/patches/optional/tests"
fi

if ((include_macos)) && [[ -d "$repo_root/patches/macos" ]]; then
    apply_patches "$repo_root/patches/macos"
fi

echo "Prepared QtScript source at $source_dir"
