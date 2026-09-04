#!/usr/bin/env bash
# Applies the QtScript Qt 6 patch series to a clean checkout of the
# QtScript 5.15.19 release. Files that no patch may own (the QuickJS-NG
# backend and the CMake entry points) are copied from overlay/ first.
#
# Usage: apply-patches.sh SOURCE_DIR [--include-ported-tests]
#   SOURCE_DIR            work tree to prepare (cloned if missing)
#   --include-ported-tests  also apply patches/optional/tests

set -euo pipefail

base_commit="bcd7cae6215df8f1c8b45a338f3327da51edeaff"
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
        # Retained for the legacy JSC backend only; the QuickJS macOS build
        # intentionally does not pass this (see patches/macos/).
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
overlay_dir="$repo_root/overlay"

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

    mkdir -p "$source_dir"
    git -C "$source_dir" init
    git -C "$source_dir" remote add origin "$repository"
    git -C "$source_dir" fetch --depth 1 origin "$base_commit"
    git -C "$source_dir" checkout --detach FETCH_HEAD
fi

if [[ -d "$source_dir/.git/rebase-apply" ]]; then
    echo "SourceDir has an interrupted git am session (.git/rebase-apply). Resolve or abort it first: $source_dir" >&2
    exit 1
fi

if [[ -n "$(git -C "$source_dir" status --porcelain)" ]]; then
    echo "The QtScript source tree has uncommitted changes: $source_dir" >&2
    exit 1
fi

overlay_fingerprint() {
    local mode="$1" rel
    (cd "$overlay_dir" && find . -type f | sort) | while IFS= read -r file; do
        rel="${file#./}"
        if [[ "$mode" == tests ]]; then
            [[ "$rel" == tests/* ]] || continue
        elif [[ "$rel" == tests/* ]]; then
            continue
        fi
        git hash-object "$overlay_dir/$rel"
    done
}

# Copy overlay-managed files into the source tree. These files are owned by
# this repository, not by the patch series, so no patch references them.
# Copies are skipped once the owning series is applied (its marker exists),
# so a prepared tree is never clobbered.
copy_overlay() {
    local marker_name="$1" mode="$2" rel
    [[ -f "$source_dir/.git/$marker_name" ]] && return 0
    while IFS= read -r -d '' file; do
        rel="${file#./}"
        if [[ "$mode" == tests ]]; then
            [[ "$rel" == tests/* ]] || continue
        elif [[ "$rel" == tests/* ]]; then
            continue
        fi
        mkdir -p "$source_dir/$(dirname "$rel")"
        cp "$overlay_dir/$rel" "$source_dir/$rel"
    done < <(cd "$overlay_dir" && find . -type f -print0)
}

# The overlay files are untracked in the source tree; excluding them keeps
# `git status --porcelain` clean so re-runs pass the dirty check.
write_overlay_exclude() {
    (cd "$overlay_dir" && find . -type f | sed 's|^\./|/|') \
        > "$source_dir/.git/info/exclude"
}

apply_patches() {
    local patch_dir="$1"
    local marker_name="$2"
    local required_head="${3:-}"
    local overlay_mode="${4:-}"
    shopt -s nullglob
    patches=("$patch_dir"/*.patch)
    shopt -u nullglob
    if ((${#patches[@]} == 0)); then
        echo "No patches were found in $patch_dir" >&2
        exit 1
    fi

    local fingerprint marker head
    fingerprint="$(git hash-object "${patches[@]}")"
    if [[ -n "$overlay_mode" ]]; then
        fingerprint+=$'\n'
        fingerprint+="$(overlay_fingerprint "$overlay_mode")"
    fi
    marker="$source_dir/.git/$marker_name"
    if [[ -f "$marker" ]]; then
        if [[ "$(<"$marker")" != "$fingerprint" ]]; then
            echo "The patch series changed after it was applied. Use a fresh SourceDir: $source_dir" >&2
            exit 1
        fi
        return
    fi
    if [[ -n "$required_head" ]]; then
        head="$(git -C "$source_dir" rev-parse HEAD)"
        if [[ "$head" != "$required_head" ]]; then
            echo "SourceDir is not at the pinned QtScript base $required_head and has no patch marker: $source_dir" >&2
            exit 1
        fi
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
    printf '%s' "$fingerprint" >"$marker"
}
copy_overlay qtscript-quickjs-patches all
if ((include_tests)); then
    copy_overlay qtscript-optional-test-patches tests
fi
write_overlay_exclude


apply_patches "$repo_root/patches/quickjs" qtscript-quickjs-patches "$base_commit" all

if ((include_tests)); then
    apply_patches "$repo_root/patches/optional/tests" qtscript-optional-test-patches "" tests
fi

if ((include_macos)) && [[ -d "$repo_root/patches/macos" ]]; then
    apply_patches "$repo_root/patches/macos"
fi

echo "Prepared QtScript source at $source_dir"
