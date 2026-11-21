#!/usr/bin/env bash
# Share the same physical-memory policy across installer and normal builds.
ndnsf_host_memory_bytes() {
    local memory_kib
    memory_kib=$(awk '$1 == "MemTotal:" { print $2; exit }' /proc/meminfo)
    [[ $memory_kib =~ ^[0-9]+$ ]] || return 1
    printf '%s\n' "$((memory_kib * 1024))"
}

ndnsf_host_build_job_ceiling() {
    local memory_bytes
    if ! memory_bytes=$(ndnsf_host_memory_bytes); then
        printf '6\n'
    elif (( memory_bytes <= 12000000000 )); then
        printf '4\n'
    elif (( memory_bytes < 100000000000 )); then
        printf '6\n'
    else
        printf '0\n'
    fi
}

ndnsf_default_host_build_jobs() {
    local ceiling
    ceiling=$(ndnsf_host_build_job_ceiling)
    if (( ceiling == 0 )); then
        nproc
    else
        printf '%s\n' "$ceiling"
    fi
}

ndnsf_host_build_jobs_allowed() {
    [[ $1 =~ ^[1-9][0-9]*$ ]] || return 1
    local ceiling
    ceiling=$(ndnsf_host_build_job_ceiling)
    if (( ceiling == 0 )); then
        return 0
    fi
    case "$1" in
        1|2|4) return 0 ;;
        6) (( ceiling == 6 )) ;;
        *) return 1 ;;
    esac
}

ndnsf_report_invalid_host_build_jobs() {
    printf '%s\n' 'Invalid host build job count. Use 1, 2 or 4 up to 12 GB RAM.' \
        'Use 1, 2, 4 or 6 above 12 GB and below 100 GB or with unknown RAM.' \
        'At 100 GB or more, use any positive integer.' >&2
}
