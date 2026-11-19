#!/usr/bin/env bash
# Launch a host build with the requested Waf parallelism and no imposed
# process-tree memory limit. The kernel manages memory pressure normally.
set -euo pipefail

build_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if (( $# == 0 )); then
    set -- build --jobs=6
fi

build_command=()
build_working_dir=$PWD
if [[ $1 == run ]]; then
    shift
    [[ ${1:-} == -- ]] || { echo 'Usage: build-safe.sh run -- COMMAND [ARGS...]' >&2; exit 2; }
    shift
    (( $# )) || { echo 'A build command is required' >&2; exit 2; }
    build_command=("$@")
elif [[ $1 == build || $1 == asan ]]; then
    build_mode=$1
    shift
    build_working_dir="$build_repo/build"
    build_lock=.lock-waf_linux_build
    if [[ $build_mode == asan ]]; then
        build_working_dir="$build_repo/build-ASAN-UBSAN"
        build_lock=.lock-waf_build_asan
    fi
    build_jobs_seen=false
    build_jobs_pending=false
    build_jobs=6
    build_targets=""
    build_targets_pending=false
    for build_arg in "$@"; do
        build_jobs_value=
        if $build_targets_pending; then
            build_targets=$build_arg
            build_targets_pending=false
            continue
        elif $build_jobs_pending; then
            build_jobs_value=$build_arg
            build_jobs_pending=false
        else
            case "$build_arg" in
                -j|--jobs) build_jobs_pending=true; continue ;;
                --jobs=*) build_jobs_value=${build_arg#--jobs=} ;;
                -j*) build_jobs_value=${build_arg#-j} ;;
                --targets) build_targets_pending=true; continue ;;
                --targets=*) build_targets=${build_arg#--targets=}; continue ;;
                --out|--out=*|--top|--top=*)
                    echo 'The build launcher owns the Waf source and output directories' >&2
                    exit 2 ;;
                *) continue ;;
            esac
        fi
        if [[ $build_jobs_value != 1 && $build_jobs_value != 2 &&
              $build_jobs_value != 4 && $build_jobs_value != 6 ]]; then
            echo 'Host builds accept --jobs=1, --jobs=2, --jobs=4 or --jobs=6' >&2
            exit 2
        fi
        build_jobs=$build_jobs_value
        build_jobs_seen=true
    done
    if $build_jobs_pending || $build_targets_pending; then
        echo 'Missing Waf option value' >&2
        exit 2
    fi
    if ! $build_jobs_seen; then
        set -- "$@" --jobs=6
    fi
    if [[ ! -f "$build_working_dir/$build_lock" ]]; then
        echo "Configure the maintained tree before using $build_mode: $build_working_dir" >&2
        exit 2
    fi
    build_command=(python3 "$build_repo/scripts/build-storage.py" run
        --build-dir "$build_working_dir" --profile "$build_mode"
        --targets "$build_targets" --jobs "$build_jobs" --
        env "WAFLOCK=$build_lock" "$build_repo/waf" --out="$build_working_dir" build "$@")
else
    echo 'build-safe.sh accepts build, asan, or run -- COMMAND' >&2
    exit 2
fi

# Serialize independent build invocations that might share a Waf output tree.
exec 9>"${XDG_RUNTIME_DIR:-/run/user/$UID}/ndnsf-build.lock"
if ! flock -n 9; then
    echo 'Another host build launcher is active' >&2
    exit 75
fi

echo 'Host build memory limit: none'
cd -- "$build_working_dir"
exec "${build_command[@]}"
