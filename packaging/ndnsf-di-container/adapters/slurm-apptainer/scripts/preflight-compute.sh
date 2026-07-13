#!/bin/sh
set -eu
scratch=''; gpu_type=''; gpu_count=''
while [ "$#" -gt 0 ]; do case "$1" in --scratch) scratch=$2;shift 2;;--gpu-type) gpu_type=$2;shift 2;;--gpu-count) gpu_count=$2;shift 2;;*) exit 2;;esac;done
[ -n "${SLURM_JOB_ID:-}" ] || { echo COMPUTE_PREFLIGHT_REQUIRES_SLURM >&2; exit 3; }
case "$scratch" in /tmp/ndnsf-di-*) ;; *) echo COMPUTE_SCRATCH_POLICY_INVALID >&2;exit 3;;esac
mkdir -p "$scratch/evidence"; apptainer version > "$scratch/evidence/apptainer-version.txt"
[ "$gpu_count" -gt 0 ] && nvidia-smi --query-gpu=index,uuid,name,memory.total,driver_version --format=csv,noheader,nounits > "$scratch/evidence/host-gpu.csv"
printf 'gpuType=%s\ngpuCount=%s\nslurmJobGpus=%s\ncudaVisibleDevices=%s\n' "$gpu_type" "$gpu_count" "${SLURM_JOB_GPUS:-}" "${CUDA_VISIBLE_DEVICES:-}" > "$scratch/evidence/allocation.env"
