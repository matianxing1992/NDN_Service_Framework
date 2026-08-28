#!/usr/bin/env bash
# Spec170 D1: one allocated GPU, one native Provider, one authenticated request.
# The exact SIF and the candidate-bound workload are supplied through exports.
# This gate intentionally uses the same preflight/run-container tool bundle as
# D0; only the allocation and --nv path differ.
#SBATCH --job-name=spec170-d1-current
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=gpu:1
# itiger07 had a bounded exact-SIF launch failure (Job 197160) and is not
# qualified for this candidate until a fresh runtime preflight passes.
#SBATCH --exclude=itiger07
#SBATCH --time=00:30:00

set -euo pipefail
unset APPTAINERENV_HOME SINGULARITYENV_HOME
: "${SPEC170_PROJECT_ROOT:?}"
: "${SPEC170_TOOL_ROOT:?}"
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_IDENTITY_ROOT:?}"
: "${SPEC170_WORKLOAD:?}"
test "${SPEC170_GATE:-d1-single}" = d1-single
case "$SPEC170_WORKLOAD" in /*) ;; *) echo SPEC170_WORKLOAD_MUST_BE_CONTAINER_ABSOLUTE >&2; exit 2 ;; esac
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
mkdir -p "$scratch/evidence"
trap 'rm -rf "$scratch"' EXIT
"$SPEC170_TOOL_ROOT/preflight-compute.sh" \
  --scratch "$scratch" --gpu-type allocated --gpu-count 1
"$SPEC170_TOOL_ROOT/run-container.sh" \
  --sif "$SPEC170_RUNTIME_SIF" --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" --gpu-count 1 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" --identity "$SPEC170_IDENTITY_ROOT" -- \
  "$SPEC170_WORKLOAD"
echo "SPEC170_D1_CURRENT_GPU_PASS jobId=${SLURM_JOB_ID}"
