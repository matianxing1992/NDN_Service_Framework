#!/usr/bin/env bash
# Non-gate environment probe. It does not run NDNSF, Apptainer, or inference.
# Keep this separate from the immutable D0-D2 gate identities.
#SBATCH --job-name=spec170-env-probe
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --time=00:05:00

set -euo pipefail

echo "HOST=$(hostname)"
echo "USER=$(id -un)"
echo "JOB=${SLURM_JOB_ID}"
echo "PARTITION=${SLURM_JOB_PARTITION}"
command -v apptainer
apptainer version
echo "SLURM_GPUS_ON_NODE=${SLURM_GPUS_ON_NODE:-}"
echo "CUDA_VISIBLE_DEVICES=${CUDA_VISIBLE_DEVICES:-}"
for tool in podman buildah docker fakeroot; do
  printf '%s=' "$tool"
  command -v "$tool" || true
done
podman --version 2>&1 || true
buildah --version 2>&1 || true
fakeroot --version 2>&1 | head -1 || true
test -d /project/tma1/ndnsf-di
test -w /project/tma1/ndnsf-di
test -d /tmp
test -w /tmp
df -h /project/tma1/ndnsf-di /tmp
echo "SPEC170_ENV_PROBE_PASS"
