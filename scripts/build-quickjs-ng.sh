#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
configuration="Release"
work_root="${QUICKJS_NG_WORK_ROOT:-$repo_root/.work/quickjs-ng}"
quickjs_source="${QUICKJS_NG_SOURCE_DIR:-$repo_root/third_party/quickjs-ng}"
host_os="$(uname -s)"
case "$host_os" in
    Linux)
        parallel="$(nproc)"
        ;;
    Darwin)
        parallel="$(sysctl -n hw.logicalcpu)"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        parallel="$(nproc)"
        ;;
    *)
        echo "Unsupported operating system: $host_os" >&2
        exit 1
        ;;
esac
generator="Ninja"
architectures=""

usage() {
    echo "Usage: $0 [--configuration Debug|Release|All] [--work-root PATH] [--quickjs-source PATH] [--parallel N] [--generator NAME] [--architectures LIST]"
}

while (($#)); do
    case "$1" in
        --configuration)
            if (($# < 2)); then
                echo "--configuration requires Debug, Release, or All." >&2
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
        --quickjs-source)
            if (($# < 2)); then
                echo "--quickjs-source requires a path." >&2
                exit 2
            fi
            quickjs_source="$2"
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
        --generator)
            if (($# < 2)); then
                echo "--generator requires a CMake generator name." >&2
                exit 2
            fi
            generator="$2"
            shift 2
            ;;
        --architectures)
            if (($# < 2)); then
                echo "--architectures requires a CMake architecture list." >&2
                exit 2
            fi
            architectures="$2"
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

case "$configuration" in
    Debug|Release)
        configurations=("$configuration")
        ;;
    All)
        configurations=(Debug Release)
        ;;
    *)
        echo "Configuration must be Debug, Release, or All." >&2
        exit 2
        ;;
esac

if [[ ! "$parallel" =~ ^[0-9]+$ || "$parallel" -lt 1 ]]; then
    echo "Parallel must be a positive integer." >&2
    exit 2
fi

for command in cmake git; do
    if ! command -v "$command" >/dev/null; then
        echo "$command was not found on PATH." >&2
        exit 1
    fi
done

if [[ ! -f "$quickjs_source/CMakeLists.txt" || ! -f "$quickjs_source/quickjs.h" ]]; then
    echo "QuickJS-NG submodule is not initialized: $quickjs_source" >&2
    echo "Run 'git submodule update --init --recursive' first." >&2
    exit 1
fi

expected_commit="954dc53628e36891f93c359aa60895c2ae3dac6b"
actual_commit="$(git -C "$quickjs_source" rev-parse HEAD)"
if [[ "$actual_commit" != "$expected_commit" ]]; then
    echo "QuickJS-NG revision mismatch: expected $expected_commit, got $actual_commit." >&2
    exit 1
fi

quickjs_patch_dir="$repo_root/patches/quickjs-ng"
if [[ -d "$quickjs_patch_dir" ]]; then
    while IFS= read -r -d '' patch; do
        if git -C "$quickjs_source" apply --check -R --ignore-space-change --ignore-whitespace "$patch" 2>/dev/null; then
            continue
        fi
        if ! git -C "$quickjs_source" apply --check --ignore-space-change --ignore-whitespace "$patch" 2>/dev/null; then
            echo "QuickJS-NG compatibility patch cannot be applied: $(basename "$patch")" >&2
            exit 1
        fi
        if ! git -C "$quickjs_source" apply --ignore-space-change --ignore-whitespace "$patch"; then
            echo "QuickJS-NG compatibility patch failed: $(basename "$patch")" >&2
            exit 1
        fi
    done < <(find "$quickjs_patch_dir" -maxdepth 1 -type f -name '*.patch' -print0 | sort -z)
fi

if ! grep -Eq '^#define QJS_VERSION_MAJOR 0$' "$quickjs_source/quickjs.h" ||
   ! grep -Eq '^#define QJS_VERSION_MINOR 16$' "$quickjs_source/quickjs.h" ||
   ! grep -Eq '^#define QJS_VERSION_PATCH 1$' "$quickjs_source/quickjs.h"; then
    echo "QuickJS-NG source is not version 0.16.1: $quickjs_source/quickjs.h" >&2
    exit 1
fi

mkdir -p "$work_root"
work_root="$(cd "$work_root" && pwd)"
quickjs_source="$(cd "$quickjs_source" && pwd)"

for build_configuration in "${configurations[@]}"; do
    configuration_root="$work_root/$build_configuration"
    build_dir="$configuration_root/build"
    install_dir="$configuration_root/install"

    mkdir -p "$configuration_root" "$install_dir"

    configure_args=(
        -S "$quickjs_source"
        -B "$build_dir"
        -G "$generator"
        "-DCMAKE_INSTALL_PREFIX=$install_dir"
        -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DQJS_BUILD_LIBC=OFF
        -DQJS_BUILD_EXAMPLES=OFF
        -DQJS_ENABLE_INSTALL=OFF
        -DQJS_BUILD_WERROR=OFF
    )
    if [[ -n "$architectures" && "$host_os" == Darwin ]]; then
        configure_args+=("-DCMAKE_OSX_ARCHITECTURES=$architectures")
    fi
    build_args=(--build "$build_dir" --target qjs qjs_exe api-test --parallel "$parallel")
    if [[ "$generator" == *"Multi-Config"* ]]; then
        configure_args+=("-DCMAKE_CONFIGURATION_TYPES=$build_configuration")
        build_args+=(--config "$build_configuration")
    else
        configure_args+=("-DCMAKE_BUILD_TYPE=$build_configuration")
    fi

    echo "Configuring QuickJS-NG 0.16.1 $build_configuration"
    cmake "${configure_args[@]}"

    cmake "${build_args[@]}"

    qjs_library="$build_dir/libqjs.a"
    qjs_executable="$build_dir/qjs"
    api_test="$build_dir/api-test"
    if [[ "$generator" == *"Multi-Config"* ]]; then
        qjs_library="$build_dir/$build_configuration/libqjs.a"
        qjs_executable="$build_dir/$build_configuration/qjs"
        api_test="$build_dir/$build_configuration/api-test"
    fi

    if [[ ! -f "$qjs_library" ]]; then
        echo "QuickJS-NG static library was not produced: $qjs_library" >&2
        exit 1
    fi
    if [[ ! -x "$qjs_executable" ]]; then
        echo "QuickJS-NG CLI was not produced: $qjs_executable" >&2
        exit 1
    fi
    if [[ ! -x "$api_test" ]]; then
        echo "QuickJS-NG api-test was not produced: $api_test" >&2
        exit 1
    fi
    printf 'commit=%s\nconfiguration=%s\n' \
        "$expected_commit" "$build_configuration" \
        >"$(dirname "$qjs_library")/.qtscript-quickjs-build"

    if find "$build_dir" -type f \( -name 'qjs.dll' -o -name 'qjs.so' -o -name 'qjs.so.*' -o -name 'qjs.dylib' -o -name 'qjs.dylib.*' -o -name 'libqjs.so' -o -name 'libqjs.so.*' -o -name 'libqjs.dylib' -o -name 'libqjs.dylib.*' \) -print -quit | grep -q .; then
        echo "QuickJS-NG produced a shared engine library under $build_dir." >&2
        exit 1
    fi

    echo "Running QuickJS-NG api-test ($build_configuration)"
    "$api_test"
    echo "Running QuickJS-NG expression smoke ($build_configuration)"
    "$qjs_executable" -e 'if (1 + 2 !== 3 || typeof BigInt !== "function") throw new Error("QuickJS-NG expression smoke failed"); console.log("QuickJS-NG expression smoke passed")'
done

echo "QuickJS-NG 0.16.1 static build passed: ${configurations[*]}"
