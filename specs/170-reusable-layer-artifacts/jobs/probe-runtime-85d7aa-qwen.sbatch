#!/usr/bin/env bash
# Exact-SIF standalone Qwen3 reference. Override CANDIDATE_DIR for a new
# release.
#SBATCH --job-name=spec170-qwen-85d7aa
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --gres=gpu:1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --time=00:15:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-qwen-85d7aa-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-qwen-85d7aa-%j.err

CANDIDATE_DIR=${CANDIDATE_DIR:-runtime-85d7aa475217cabbcfa92ad28612db99cd36e77b}
set -euo pipefail
sif=/project/tma1/ndnsf-di/releases/spec170-${CANDIDATE_DIR}/runtime.sif
model=/project/tma1/ndnsf-di/inputs/spec166/model
prompt=/project/tma1/ndnsf-di/evidence/spec170/${CANDIDATE_DIR}/qwen-prompt-set.json
outdir=/project/tma1/ndnsf-di/evidence/spec170/${CANDIDATE_DIR}/qwen-results
output=/output/qwen-reference-${SLURM_JOB_ID}.json
host_output="$outdir/qwen-reference-${SLURM_JOB_ID}.json"
mkdir -p "$outdir"
test -r "$prompt"
sha256sum "$sif" "$prompt"
apptainer exec --nv --cleanenv --containall --home /tmp --pwd /tmp \
  --env SLURM_JOB_ID="$SLURM_JOB_ID" --bind "$model:$model:ro" \
  --bind "$(dirname "$prompt"):$(dirname "$prompt"):ro" \
  --bind "$outdir:/output:rw" \
  "$sif" python3 /opt/ndnsf/bin/run-qwen-reference.py \
  --model "$model" --prompt-file "$prompt" --max-new-tokens 2 \
  --seed 109 --output "$output"
test -s "$host_output"
sha256sum "$host_output"
printf 'SPEC170_RUNTIME_QWEN_PASS candidate=%s job=%s output=%s\n' "$CANDIDATE_DIR" "$SLURM_JOB_ID" "$host_output"
