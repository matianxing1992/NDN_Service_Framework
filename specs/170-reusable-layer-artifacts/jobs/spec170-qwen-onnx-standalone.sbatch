#!/usr/bin/env bash
# Spec170 standalone real-Qwen artifact gate.  This is a prerequisite to the
# NDNSF V3 network path, not a distributed-inference acceptance result.
#SBATCH --job-name=spec170-qwen-standalone
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=48G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/logs/spec170-qwen-standalone-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/logs/spec170-qwen-standalone-%j.err

set -euo pipefail
: "${SPEC170_DEVICE:=cpu}"
CANDIDATE_SOURCE=${CANDIDATE_SOURCE:-2a7b847bbeba1b628a1a9e2a146b5411f1df5556}
BASE=/project/tma1/ndnsf-di
SIF="$BASE/releases/spec170-runtime-$CANDIDATE_SOURCE/runtime.sif"
INPUT="$BASE/inputs/spec166"
ROOT="$BASE/evidence/spec170/standalone-qwen-$CANDIDATE_SOURCE"
SCRIPT="$ROOT/spec170_qwen_onnx_standalone.py"
MANIFEST="$INPUT/qwen-onnx-service-manifest.json"
SCRATCH="/tmp/spec170-qwen-standalone-$SLURM_JOB_ID"
OUT="$ROOT/run-$SLURM_JOB_ID"
mkdir -p "$ROOT" "$SCRATCH" "$OUT"
trap 'rc=$?; echo "SPEC170_QWEN_STANDALONE_TERMINAL exit=$rc job=$SLURM_JOB_ID"; exit "$rc"' EXIT

if [ "$SPEC170_DEVICE" != cpu ]; then
  nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
    --format=csv,noheader >"$OUT/host-gpu.txt"
  GPU_ARGS=(--nv)
else
  GPU_ARGS=()
fi

for required in "$SIF" "$MANIFEST" "$SCRIPT"; do
  test -s "$required"
done
apptainer exec "${GPU_ARGS[@]}" --cleanenv --containall --no-home \
  --env HOME=/scratch,PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1 \
  --bind "$INPUT/model:/models:ro" \
  --bind "$INPUT/artifacts:/artifacts:ro" \
  --bind "$MANIFEST:/manifest/qwen-onnx-service-manifest.json:ro" \
  --bind "$SCRIPT:/scripts/spec170_qwen_onnx_standalone.py:ro" \
  --bind "$OUT:/output:rw" \
  --bind "$SCRATCH:/scratch:rw" \
  "$SIF" python3 /scripts/spec170_qwen_onnx_standalone.py \
    --manifest /manifest/qwen-onnx-service-manifest.json \
    --artifact-root /artifacts --model /models \
    --device "$SPEC170_DEVICE" --max-new-tokens 2 --output-dir /output

test -s "$OUT/summary.json"
echo "SPEC170_QWEN_STANDALONE_PASS job=$SLURM_JOB_ID device=$SPEC170_DEVICE"
