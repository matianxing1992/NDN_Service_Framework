#!/usr/bin/env bash
#SBATCH --job-name=ndnsf175-diag3gpu
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gres=gpu:rtx_6000:3
#SBATCH --mem=96G
#SBATCH --time=00:30:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -euo pipefail

# This job proves one real three-Provider CUDA execution quickly.  It is not a
# G6 qualification and must never be used to close T026 or a formal campaign.
export SPEC175_GATE=diagnostic-multi-provider
SPEC175_JOB_ROOT="${SPEC175_JOB_ROOT:-${SLURM_SUBMIT_DIR:-$PWD}/packaging/ndnsf-di-container/jobs/spec175}"
export SPEC175_SIF="${SPEC175_LOCAL_SIF:?set SPEC175_LOCAL_SIF}"
export SPEC175_SIF_SHA256="${SPEC175_LOCAL_SIF_SHA256:?set SPEC175_LOCAL_SIF_SHA256}"
export SPEC175_BUNDLE="${SPEC175_BUNDLE:?set SPEC175_BUNDLE}"
export SPEC175_MODEL_ROOT="${SPEC175_REMOTE_MODEL_ROOT:?set SPEC175_REMOTE_MODEL_ROOT}"
export SPEC175_STAGE_MANIFEST_OVERRIDE="$SPEC175_BUNDLE/qwen36-stage-manifest.json"
export SPEC175_SVS_PERIODIC_SYNC_MS=1000
export SPEC175_OUTPUT="${SPEC175_OUTPUT:-$PWD/spec175-diagnostic-$SLURM_JOB_ID}"
exec "$SPEC175_JOB_ROOT/run-streamed-generation.sh"
