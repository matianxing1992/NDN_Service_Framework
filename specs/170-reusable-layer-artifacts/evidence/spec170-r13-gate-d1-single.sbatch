#!/usr/bin/env bash
# Exact r13 SIF, real one-Provider CUDA path.
#SBATCH --job-name=spec170-r13-d1
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=gpu:1
#SBATCH --time=00:30:00
#SBATCH --exclude=itiger07
#SBATCH --output=/project/tma1/ndnsf-di/releases/spec170-runtime-31d9f547-post-selection-v11-20260817/evidence/d1-%j.out

set -euo pipefail
unset APPTAINERENV_HOME APPTAINERENV_PWD SINGULARITYENV_HOME SINGULARITYENV_PWD

project=/project/tma1/ndnsf-di
release="$project/releases/spec170-runtime-31d9f547-post-selection-v11-20260817"
sif="$release/runtime.sif"
sif_sha=sha256:4e29b0fdc14eba8172fb927dcea07676e807f1b577ea76a82b786b407cbc2840
identity="$project/identity/spec170"
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
preflight="$release/operator-tools/preflight-compute.sh"
run_container="$release/operator-tools/run-container.sh"

mkdir -p "$scratch/evidence" "$release/evidence/d1-preflight-${SLURM_JOB_ID}"
trap 'rm -rf "$scratch"' EXIT

echo "R13_D1_STEP=compute-contract node=$(hostname -s)"
actual_apptainer=$(apptainer version | tee "$scratch/evidence/apptainer-version.txt" | sed -E 's/[^0-9]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
rpm -q apptainer | tee "$scratch/evidence/apptainer-package.txt"
test "$actual_apptainer" = 1.5.3
test -x "$preflight" && test -x "$run_container" && test -d "$identity"

echo R13_D1_STEP=preflight
"$preflight" --scratch "$scratch" --gpu-type allocated --gpu-count 1

echo R13_D1_STEP=network-workload
"$run_container" \
  --sif "$sif" --sif-sha256 "$sif_sha" --gpu-count 1 \
  --project "$project" --scratch "$scratch" --identity "$identity" \
  --release-bind "$release" --evidence "$release/evidence" -- \
  /release/network-bundle-gpu/spec170-d1-current-sif-workload.sh

cp -a "$scratch/evidence/." "$release/evidence/d1-preflight-${SLURM_JOB_ID}/"
echo "SPEC170_R13_D1_CUDA_PASS jobId=${SLURM_JOB_ID} node=$(hostname -s)"
