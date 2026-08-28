#!/usr/bin/env bash
# Spec170 D0 CPU network gate using the exact current tool bundle.
# The tool root is separate from the shared historical Tiger packaging tree.
# This prevents an old scratch-policy wrapper from being mixed with the r5 SIF.
#SBATCH --job-name=spec170-d0-v1
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=00:30:00

set -euo pipefail
unset APPTAINERENV_HOME SINGULARITYENV_HOME
: "${SPEC170_PROJECT_ROOT:?}"
: "${SPEC170_TOOL_ROOT:?}"
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_IDENTITY_ROOT:?}"
: "${SPEC170_WORKLOAD:?}"
test "${SPEC170_GATE:-d0-cpu}" = d0-cpu
case "$SPEC170_WORKLOAD" in /*) ;; *) echo SPEC170_WORKLOAD_MUST_BE_CONTAINER_ABSOLUTE >&2; exit 2 ;; esac
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
mkdir -p "$scratch/evidence"
trap 'rm -rf "$scratch"' EXIT
"$SPEC170_TOOL_ROOT/preflight-compute.sh" \
  --scratch "$scratch" --gpu-type none --gpu-count 0
"$SPEC170_TOOL_ROOT/run-container.sh" \
  --sif "$SPEC170_RUNTIME_SIF" --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" --gpu-count 0 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" --identity "$SPEC170_IDENTITY_ROOT" -- \
  "$SPEC170_WORKLOAD"
echo "SPEC170_D0_CPU_PASS jobId=${SLURM_JOB_ID} toolRoot=${SPEC170_TOOL_ROOT}"
