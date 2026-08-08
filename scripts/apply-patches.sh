#!/usr/bin/env bash
# Applies the QtScript Qt 6 patch series to a clean checkout of the pinned
# KDE baseline, verifying the exact source tree after each stage.
#
# Usage: apply-patches.sh SOURCE_DIR [--include-ported-tests]
#   SOURCE_DIR            work tree to prepare (cloned if missing)
#   --include-ported-tests  also apply patches/optional/tests (stage 2)

set -euo pipefail

base_revision="bcd7cae6215df8f1c8b45a338f3327da51edeaff"
repository="https://invent.kde.org/qt/qt/qtscript.git"

source_dir=""
include_tests=0

while (($#)); do
    case "$1" in
        --include-ported-tests)
            include_tests=1
            shift
            ;;
        -*)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 SOURCE_DIR [--include-ported-tests]" >&2
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
    echo "Usage: $0 SOURCE_DIR [--include-ported-tests]" >&2
    exit 2
fi
source_dir="$(cd "$source_dir" 2>/dev/null && pwd || echo "$source_dir")"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v git >/dev/null; then
    echo "git was not found on PATH." >&2
    exit 1
fi

if [[ ! -d "$source_dir/.git" ]]; then
    if [[ -d "$source_dir" ]]; then
        if [[ -n "$(ls -A "$source_dir")" ]]; then
            echo "SourceDir exists and is not empty: $source_dir" >&2
            exit 1
        fi
    else
        mkdir -p "$(dirname "$source_dir")"
    fi

    git clone --no-checkout "$repository" "$source_dir"
    git -C "$source_dir" checkout --detach "$base_revision"
fi

if [[ -d "$source_dir/.git/rebase-apply" ]]; then
    echo "SourceDir has an interrupted git am session (.git/rebase-apply). Resolve or abort it first: $source_dir" >&2
    exit 1
fi

if [[ -n "$(git -C "$source_dir" status --porcelain)" ]]; then
    echo "The QtScript source tree has uncommitted changes: $source_dir" >&2
    exit 1
fi

tree="$(git -C "$source_dir" rev-parse 'HEAD^{tree}')"
stages=(
    "Qt 5.15.19 baseline|9f515614bafcf1b8bf6741e77e0cded7ebe6b5f5|"
    "minimal Qt 6 core port|730db66c641b124db4a21a8dc1b0883f6d99437f|patches"
    "ported compatibility tests|416ddcf14e38460bd94edc322cfaa1fae71c4da2|patches/optional/tests"
)

current_stage=-1
for i in "${!stages[@]}"; do
    IFS='|' read -r _name _tree _dir <<< "${stages[$i]}"
    if [[ "$tree" == "$_tree" ]]; then
        current_stage=$i
        break
    fi
done
if ((current_stage < 0)); then
    echo "Unexpected QtScript source tree $tree. Use a clean pinned baseline or a tree prepared by this script." >&2
    exit 1
fi

target_stage=1
if ((include_tests)); then
    target_stage=2
fi

if ((current_stage > target_stage)); then
    echo "The source already includes stage $((current_stage + 1)), which exceeds the requested stage $((target_stage + 1))." >&2
    exit 1
fi
if ((current_stage == target_stage)); then
    echo "QtScript is already prepared at tree $tree (stage $((target_stage + 1)))."
    exit 0
fi

for ((stage_index = current_stage + 1; stage_index <= target_stage; ++stage_index)); do
    IFS='|' read -r stage_name expected_tree patch_dir <<< "${stages[$stage_index]}"
    patch_dir="$repo_root/$patch_dir"
    shopt -s nullglob
    patches=("$patch_dir"/*.patch)
    shopt -u nullglob
    if ((${#patches[@]} == 0)); then
        echo "No patches were found for stage $((stage_index + 1)) in $patch_dir" >&2
        exit 1
    fi

    for patch in "${patches[@]}"; do
        echo "Applying $(basename "$patch")"
        git -C "$source_dir" \
            -c 'user.name=QtScript Qt 6 patch set' \
            -c 'user.email=qtscript-qt6@local.invalid' \
            am "$patch"
    done

    tree="$(git -C "$source_dir" rev-parse 'HEAD^{tree}')"
    if [[ "$tree" != "$expected_tree" ]]; then
        echo "Patch verification failed after $stage_name: tree=$tree, expected=$expected_tree" >&2
        exit 1
    fi
done

echo "Prepared QtScript at $source_dir (stage $((target_stage + 1)), tree $tree)"
