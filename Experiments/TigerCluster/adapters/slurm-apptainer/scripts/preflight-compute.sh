#!/bin/sh
set -eu
scratch=''; gpu_type=''; gpu_count=''
while [ "$#" -gt 0 ]; do case "$1" in --scratch) scratch=$2;shift 2;;--gpu-type) gpu_type=$2;shift 2;;--gpu-count) gpu_count=$2;shift 2;;*) exit 2;;esac;done
[ -n "${SLURM_JOB_ID:-}" ] || { echo COMPUTE_PREFLIGHT_REQUIRES_SLURM >&2; exit 3; }
# The canonical batch template uses a job-bound basename with an optional
# run-id suffix.
# Keep the preflight contract aligned with run-container.sh and the topology
# supervisor, while rejecting nested paths and another job's scratch.
scratch_name=${scratch#/tmp/}
case "$scratch" in /tmp/*) ;; *) echo COMPUTE_SCRATCH_POLICY_INVALID >&2; exit 3 ;; esac
case "$scratch_name" in
  */*|'') echo COMPUTE_SCRATCH_POLICY_INVALID >&2; exit 3 ;;
  "ndnsf-di-${SLURM_JOB_ID}"|"ndnsf-di-${SLURM_JOB_ID}-"*) ;;
  *) echo COMPUTE_SCRATCH_POLICY_INVALID >&2; exit 3 ;;
esac
# The basename check alone is insufficient when /tmp contains a symlink (or
# a symlinked parent) with a job-looking name. Do not create evidence through
# such a path: the canonical container runner resolves it later and would
# otherwise observe a different scratch root in the same batch job.
scratch_input=$scratch
while [ "${scratch_input%/}" != "$scratch_input" ]; do scratch_input=${scratch_input%/}; done
scratch_real=$(readlink -f -- "$scratch" 2>/dev/null || true)
[ -n "$scratch_real" ] && [ "$scratch_real" = "$scratch_input" ] || {
  echo COMPUTE_SCRATCH_SYMLINK_FORBIDDEN >&2; exit 3;
}
mkdir -p "$scratch/evidence"
# On current iTiger compute nodes ``apptainer version`` can hang while the
# equivalent ``--version`` probe returns the installed package identity.  The
# latter is the bounded capability gate; never let a diagnostic version
# subcommand consume the allocation wall time.
if ! timeout 10s apptainer --version > "$scratch/evidence/apptainer-version.txt" 2>&1; then
  echo COMPUTE_APPTAINER_VERSION_PROBE_FAILED >&2
  exit 4
fi
[ "$gpu_count" -gt 0 ] && timeout 15s nvidia-smi --query-gpu=index,uuid,name,memory.total,driver_version --format=csv,noheader,nounits > "$scratch/evidence/host-gpu.csv"
printf 'gpuType=%s\ngpuCount=%s\nslurmJobGpus=%s\ncudaVisibleDevices=%s\n' "$gpu_type" "$gpu_count" "${SLURM_JOB_GPUS:-}" "${CUDA_VISIBLE_DEVICES:-}" > "$scratch/evidence/allocation.env"
