#!/usr/bin/env bash
#SBATCH --job-name=ndnsf175-multi
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
# The frozen Qwen3.6-27B subject was qualified on three RTX 6000 GPUs.
#SBATCH --gres=gpu:rtx_6000:3
# The three Qwen stages plus NFD/Controller/User/Provider processes have a
# qualified 96-GiB host-memory envelope.  Do not fall back to Slurm's small
# per-CPU default: that previously produced a memory-cgroup OOM before model
# readiness.
#SBATCH --mem=96G
#SBATCH --time=01:00:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -euo pipefail
export SPEC175_GATE=multi-provider
SPEC175_JOB_ROOT="${SPEC175_JOB_ROOT:-${SLURM_SUBMIT_DIR:-$PWD}/packaging/ndnsf-di-container/jobs/spec175}"
export SPEC175_SIF="$SPEC175_LOCAL_SIF"
export SPEC175_SIF_SHA256="$SPEC175_LOCAL_SIF_SHA256"
export SPEC175_BUNDLE="${SPEC175_BUNDLE:?set SPEC175_BUNDLE to the staged bundle}"
export SPEC175_MODEL_ROOT="${SPEC175_REMOTE_MODEL_ROOT:?set SPEC175_REMOTE_MODEL_ROOT to the external model root}"
export SPEC175_OUTPUT="${SPEC175_OUTPUT:-$PWD/spec175-multi-$SLURM_JOB_ID}"
exec "$SPEC175_JOB_ROOT/run-streamed-generation.sh"
