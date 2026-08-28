#!/usr/bin/env bash
#SBATCH --job-name=ndnsf175-perf
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gres=gpu:rtx_5000:3
#SBATCH --time=02:00:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -euo pipefail
export SPEC175_GATE=performance
export SPEC175_SIF="$SPEC175_LOCAL_SIF"
export SPEC175_SIF_SHA256="$SPEC175_LOCAL_SIF_SHA256"
export SPEC175_BUNDLE="${SPEC175_BUNDLE:?set SPEC175_BUNDLE to the staged bundle}"
export SPEC175_OUTPUT="${SPEC175_OUTPUT:-$PWD/spec175-performance-$SLURM_JOB_ID}"
exec "$(dirname "$0")/run-streamed-generation.sh"
