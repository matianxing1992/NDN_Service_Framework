#!/usr/bin/env bash
# Spec170 T024/T033: two Providers, one GPU on each allocated node.
#SBATCH --job-name=spec170-d2b-cross-provider
#SBATCH --partition=bigTiger
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=gpu:1
#SBATCH --time=00:45:00

set -euo pipefail
: "${SPEC170_PROJECT_ROOT:?}"
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_IDENTITY_ROOT:?}"
: "${SPEC170_WORKLOAD:?}"
test "${SPEC170_GATE:-d2b-cross-provider}" = d2b-cross-provider
case "$SPEC170_WORKLOAD" in /*) ;; *) echo 'SPEC170_WORKLOAD_MUST_BE_CONTAINER_ABSOLUTE' >&2; exit 2 ;; esac
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
scratch="/tmp/${SLURM_JOB_ID}"
mkdir -p "$scratch/evidence"
trap 'rm -rf "$scratch"' EXIT
"$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/preflight-compute.sh" \
  --scratch "$scratch" --gpu-type allocated --gpu-count 1
"$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh" \
  --sif "$SPEC170_RUNTIME_SIF" --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" --gpu-count 1 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" --identity "$SPEC170_IDENTITY_ROOT" -- \
  "$SPEC170_WORKLOAD"
echo "SPEC170_D2B_CROSS_PROVIDER_PASS jobId=${SLURM_JOB_ID}"
