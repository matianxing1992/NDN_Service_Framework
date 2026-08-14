#!/usr/bin/env bash
# Exact-SIF allocated-GPU runtime probe for the 85d7aa immutable candidate.
#SBATCH --job-name=spec170-gpu-85d7aa
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --gres=gpu:1
#SBATCH --cpus-per-task=4
#SBATCH --mem=12G
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-gpu-85d7aa-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-gpu-85d7aa-%j.err

set -euo pipefail
sif=/project/tma1/ndnsf-di/releases/spec170-runtime-85d7aa475217cabbcfa92ad28612db99cd36e77b/runtime.sif
sha256sum "$sif"
apptainer exec --nv --cleanenv --containall --home /tmp --pwd /tmp \
  --env SLURM_JOB_ID="$SLURM_JOB_ID" "$sif" \
  /usr/local/bin/ndnsf-di-probe-runtime --mode allocated-gpu
printf 'SPEC170_RUNTIME_85D7AA_GPU_PASS job=%s\n' "$SLURM_JOB_ID"
