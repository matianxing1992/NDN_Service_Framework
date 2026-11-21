#!/usr/bin/env bash
# Share the same physical-memory policy across installer and normal builds.
ndnsf_host_memory_bytes() {
    local memory_kib
    memory_kib=$(awk '$1 == "MemTotal:" { print $2; exit }' /proc/meminfo)
    [[ $memory_kib =~ ^[0-9]+$ ]] || return 1
    printf '%s\n' "$((memory_kib * 1024))"
}

ndnsf_large_memory_host() {
    local memory_bytes
    memory_bytes=$(ndnsf_host_memory_bytes) || return 1
    (( memory_bytes > 100000000000 ))
}

ndnsf_default_host_build_jobs() {
    if ndnsf_large_memory_host; then
        nproc
    else
        printf '6\n'
    fi
}

ndnsf_host_build_jobs_allowed() {
    [[ $1 =~ ^[1-9][0-9]*$ ]] || return 1
    if ndnsf_large_memory_host; then
        return 0
    fi
    case "$1" in 1|2|4|6) return 0 ;; *) return 1 ;; esac
}
