#!/usr/bin/env bash
# Spec170 T024/T034: two Providers, one GPU each, heterogeneous hybrid plan.
#SBATCH --job-name=spec170-d2h-hybrid
#SBATCH --partition=bigTiger
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=12
#SBATCH --mem=48G
#SBATCH --gres=gpu:1
#SBATCH --time=01:00:00

set -euo pipefail
: "${SPEC170_PROJECT_ROOT:?}"
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_IDENTITY_ROOT:?}"
: "${SPEC170_WORKLOAD:?}"
: "${SPEC170_HYBRID_MAPPING:?}"
test "${SPEC170_GATE:-d2h-hybrid}" = d2h-hybrid
case "$SPEC170_WORKLOAD" in /*) ;; *) echo 'SPEC170_WORKLOAD_MUST_BE_CONTAINER_ABSOLUTE' >&2; exit 2 ;; esac
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
mkdir -p "$scratch/evidence"
trap 'rm -rf "$scratch"' EXIT
printf '%s\n' "$SPEC170_HYBRID_MAPPING" > "$scratch/evidence/hybrid-mapping.txt"
"$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/preflight-compute.sh" \
  --scratch "$scratch" --gpu-type allocated --gpu-count 1
"$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh" \
  --sif "$SPEC170_RUNTIME_SIF" --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" --gpu-count 1 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" --identity "$SPEC170_IDENTITY_ROOT" -- \
  "$SPEC170_WORKLOAD"
echo "SPEC170_D2H_HYBRID_PASS jobId=${SLURM_JOB_ID} mapping=${SPEC170_HYBRID_MAPPING}"
