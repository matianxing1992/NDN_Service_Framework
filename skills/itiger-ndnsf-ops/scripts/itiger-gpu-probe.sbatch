#!/usr/bin/env bash
#SBATCH --job-name=ndnsf-gpu-probe
#SBATCH --cpus-per-task=2
#SBATCH --mem=8G
#SBATCH --nodes=1
#SBATCH --ntasks=1

set -euo pipefail

: "${PROBE_DIR:?PROBE_DIR must be exported by the submitter}"
: "${CUDA_IMAGE:?CUDA_IMAGE must be exported by the submitter}"
mkdir -p "${PROBE_DIR}"
exec > >(tee "${PROBE_DIR}/probe.log") 2>&1

scratch="/tmp/${USER}/ndnsf-gpu-probe-${SLURM_JOB_ID}"
result_file="${PROBE_DIR}/result.txt"

finish()
{
  rc=$?
  trap - EXIT
  rm -rf "${scratch}"
  if ((rc != 0)); then
    printf 'RESULT=FAIL EXIT_CODE=%s\n' "${rc}" | tee "${result_file}"
  fi
  exit "${rc}"
}
trap finish EXIT

mkdir -p "${scratch}/cache" "${scratch}/tmp"
export APPTAINER_CACHEDIR="${scratch}/cache"
export APPTAINER_TMPDIR="${scratch}/tmp"
image="${scratch}/gpu-probe.sif"

scontrol show job "${SLURM_JOB_ID}" -dd > "${PROBE_DIR}/slurm-job.txt"
printf 'PROBE_STARTED_AT=%s\n' "$(date --iso-8601=seconds)"
printf 'SLURM_JOB_ID=%s\n' "${SLURM_JOB_ID}"
printf 'HOSTNAME=%s\n' "$(hostname)"
printf 'CUDA_VISIBLE_DEVICES=%s\n' "${CUDA_VISIBLE_DEVICES:-unset}"
printf 'SLURM_JOB_GPUS=%s\n' "${SLURM_JOB_GPUS:-unset}"
printf 'SLURM_GPUS_ON_NODE=%s\n' "${SLURM_GPUS_ON_NODE:-unset}"
printf 'CUDA_IMAGE=%s\n' "${CUDA_IMAGE}"
[[ "${SLURM_GPUS_ON_NODE:-}" == "1" ]]

printf '%s\n' '=== HOST GPU ==='
nvidia-smi --query-gpu=name,uuid,driver_version,memory.total --format=csv,noheader
host_gpu_count="$(nvidia-smi --query-gpu=uuid --format=csv,noheader | wc -l)"
printf 'HOST_VISIBLE_GPU_COUNT=%s\n' "${host_gpu_count}"
[[ "${host_gpu_count}" -eq 1 ]]
nvidia-smi | sed -n '1,12p'
if command -v nvcc >/dev/null 2>&1; then nvcc --version; else echo 'HOST_NVCC=NOT_INSTALLED'; fi

printf '%s\n' '=== APPTAINER ==='
command -v apptainer
apptainer --version
command -v singularity
singularity --version

printf '%s\n' '=== COMPUTE NODE TMP ==='
df -hT /tmp
findmnt -T /tmp -o TARGET,SOURCE,FSTYPE,OPTIONS
dd if=/dev/zero of="${scratch}/write-test.bin" bs=1M count=64 conv=fsync status=none
test "$(stat -c %s "${scratch}/write-test.bin")" -eq 67108864
rm -f "${scratch}/write-test.bin"
echo 'TMP_WRITE_TEST=PASS'

printf '%s\n' '=== OCI TO SIF AND NVIDIA PASSTHROUGH ==='
apptainer pull "${image}" "${CUDA_IMAGE}"
sif_sha256="$(sha256sum "${image}" | awk '{print $1}')"
printf 'OCI_SOURCE=%s\nSIF_SHA256=%s\n' "${CUDA_IMAGE}" "${sif_sha256}" | tee "${PROBE_DIR}/container.sha256"
apptainer exec --nv "${image}" bash -lc '
  set -e
  printf "CONTAINER_HOSTNAME=%s\n" "$(hostname)"
  test -e /dev/nvidiactl
  ls -1 /dev/nvidia* | head -20
  nvidia-smi --query-gpu=name,uuid,driver_version,memory.total --format=csv,noheader
  container_gpu_count="$(nvidia-smi --query-gpu=uuid --format=csv,noheader | wc -l)"
  printf "CONTAINER_VISIBLE_GPU_COUNT=%s\n" "${container_gpu_count}"
  test "${container_gpu_count}" -eq 1
  ldconfig -p | grep -E "libcuda\.so|libnvidia-ml\.so" | head -20
  echo CONTAINER_NVIDIA_TEST=PASS
'

printf 'PROBE_FINISHED_AT=%s\n' "$(date --iso-8601=seconds)"
echo 'RESULT=PASS' | tee "${result_file}"
