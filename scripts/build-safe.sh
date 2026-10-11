#!/usr/bin/env bash
# Bound the entire build process tree so memory exhaustion stops the build
# instead of killing the desktop. Execute compiler processes as the caller.
set -euo pipefail

build_repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_unit="ndnsf-build-${UID}-$$"
build_memory_limit=${NDNSF_BUILD_MEMORY_LIMIT:-6656M}
if (( $# == 0 )); then
    set -- build --jobs=2
fi
if [[ $1 != build ]]; then
    echo 'build-safe.sh accepts only waf build arguments' >&2
    exit 2
fi
# Explicit build arguments must not silently restore Waf's CPU-based default.
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
    if [[ $build_jobs_value != 1 && $build_jobs_value != 2 && $build_jobs_value != 4 ]]; then
        echo 'Safe host builds accept only --jobs=1, --jobs=2 or explicit --jobs=4' >&2
        exit 2
    fi
    build_jobs_seen=true
done
if $build_jobs_pending; then
    echo 'Missing build job count' >&2
    exit 2
fi
if ! $build_jobs_seen; then
    set -- "$@" --jobs=2
fi
command -v systemd-run >/dev/null
sudo -n true

exec 9>"${XDG_RUNTIME_DIR:-/run/user/$UID}/ndnsf-build.lock"
if ! flock -n 9; then
    echo 'Another isolated build launcher is active' >&2
    exit 75
fi

# Account for desktop headroom before launching; a static job count alone
# does not reflect large resident tmpfs artifacts or other applications.
build_limit_bytes=$(numfmt --from=iec "$build_memory_limit")
build_available_kib=$(awk '$1 == "MemAvailable:" {print $2}' /proc/meminfo)
if (( build_available_kib * 1024 < build_limit_bytes + 2 * 1024 * 1024 * 1024 )); then
    echo "Insufficient memory headroom for a $build_memory_limit build and 2 GiB desktop reserve" >&2
    exit 75
fi
build_active_units=$(systemctl list-units --state=activating,active,deactivating --no-legend --plain \
    "ndnsf-build-${UID}-*.service")
if [[ -n $build_active_units ]]; then
    echo 'An isolated build is active or stopping; wait before starting another' >&2
    exit 75
fi

cleanup_build() {
    sudo -n systemctl stop "${build_unit}.service" >/dev/null 2>&1 || true
}
trap cleanup_build EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

build_environment=(--setenv="PATH=$PATH" --setenv="HOME=$HOME")
if [[ -n ${WAFLOCK:-} ]]; then
    build_environment+=(--setenv="WAFLOCK=$WAFLOCK")
fi
echo "Build scope: ${build_unit}.service; memory limit ${build_memory_limit}"
sudo -n systemd-run --quiet --wait --pipe --collect \
    --unit="$build_unit" --uid="$UID" --gid="$(id -g)" \
    --working-directory="$build_repo" \
    --property="MemoryLimit=$build_memory_limit" \
    --property=MemoryAccounting=yes --property=OOMScoreAdjust=600 \
    --property=OOMPolicy=stop --property=KillMode=control-group \
    --property=Nice=10 "${build_environment[@]}" \
    "$build_repo/waf" "$@"
