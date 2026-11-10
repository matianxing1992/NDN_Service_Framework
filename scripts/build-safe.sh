#!/usr/bin/env bash
# Launch a host build with the requested Waf parallelism and no imposed
# process-tree memory limit. The kernel manages memory pressure normally.
set -euo pipefail

build_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if (( $# == 0 )); then
    set -- build --jobs=6
fi

build_command=()
if [[ $1 == run ]]; then
    shift
    [[ ${1:-} == -- ]] || { echo 'Usage: build-safe.sh run -- COMMAND [ARGS...]' >&2; exit 2; }
    shift
    (( $# )) || { echo 'A build command is required' >&2; exit 2; }
    build_command=("$@")
elif [[ $1 == build ]]; then
    build_jobs_seen=false
    build_jobs_pending=false
    for build_arg in "$@"; do
        build_jobs_value=
        if $build_jobs_pending; then
            build_jobs_value=$build_arg
            build_jobs_pending=false
        else
            case "$build_arg" in
                -j|--jobs) build_jobs_pending=true; continue ;;
                --jobs=*) build_jobs_value=${build_arg#--jobs=} ;;
                -j*) build_jobs_value=${build_arg#-j} ;;
                *) continue ;;
            esac
        fi
        if [[ $build_jobs_value != 1 && $build_jobs_value != 2 &&
              $build_jobs_value != 4 && $build_jobs_value != 6 ]]; then
            echo 'Host builds accept --jobs=1, --jobs=2, --jobs=4 or --jobs=6' >&2
            exit 2
        fi
        build_jobs_seen=true
    done
    if $build_jobs_pending; then
        echo 'Missing build job count' >&2
        exit 2
    fi
    if ! $build_jobs_seen; then
        set -- "$@" --jobs=6
    fi
    build_command=("$build_repo/waf" "$@")
else
    echo 'build-safe.sh accepts waf build or run -- COMMAND' >&2
    exit 2
fi

# Serialize independent build invocations that might share a Waf output tree.
exec 9>"${XDG_RUNTIME_DIR:-/run/user/$UID}/ndnsf-build.lock"
if ! flock -n 9; then
    echo 'Another host build launcher is active' >&2
    exit 75
fi

echo 'Host build memory limit: none'
exec "${build_command[@]}"
