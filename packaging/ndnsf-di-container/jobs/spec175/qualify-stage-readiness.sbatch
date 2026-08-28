#!/usr/bin/env bash
#SBATCH --job-name=ndnsf175-stage
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:20:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -euo pipefail
export SPEC175_GATE=stage-readiness
export SPEC175_SIF="$SPEC175_LOCAL_SIF"
export SPEC175_SIF_SHA256="$SPEC175_LOCAL_SIF_SHA256"
export SPEC175_BUNDLE="${SPEC175_BUNDLE:?set SPEC175_BUNDLE to the staged bundle}"
export SPEC175_OUTPUT="${SPEC175_OUTPUT:-$PWD/spec175-stage-$SLURM_JOB_ID}"
exec "$(dirname "$0")/run-streamed-generation.sh"
