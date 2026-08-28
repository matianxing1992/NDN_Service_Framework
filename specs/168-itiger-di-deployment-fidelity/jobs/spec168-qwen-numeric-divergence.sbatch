#!/bin/bash
#SBATCH --job-name=spec168-qwen-tie
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:15:00
#SBATCH --output=/project/tma1/ndnsf-di/slurm/spec168-qwen-tie-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/slurm/spec168-qwen-tie-%j.err

set -Eeuo pipefail

: "${SPEC168_SOURCE_DIR:?}"
: "${SPEC168_SOURCE_BUNDLE_DIGEST:?}"
: "${SPEC168_RUNTIME_SIF:?}"
: "${SPEC168_RUNTIME_SIF_SHA256:?}"
: "${SPEC168_ARTIFACT_DIR:?}"
: "${SPEC168_STAGE_MANIFEST_SHA256:?}"
: "${SPEC168_DIAGNOSTIC_SCRIPT:?}"
: "${SPEC168_DIAGNOSTIC_SCRIPT_SHA256:?}"
: "${SPEC168_OUTPUT_DIR:?}"

test ! -e "$SPEC168_OUTPUT_DIR"
printf '%s  %s\n' "${SPEC168_SOURCE_BUNDLE_DIGEST#sha256:}" \
  "$SPEC168_SOURCE_DIR/source-files.sha256" | sha256sum -c -
printf '%s  %s\n' "${SPEC168_RUNTIME_SIF_SHA256#sha256:}" \
  "$SPEC168_RUNTIME_SIF" | sha256sum -c -
printf '%s  %s\n' "${SPEC168_STAGE_MANIFEST_SHA256#sha256:}" \
  "$SPEC168_ARTIFACT_DIR/stage-manifest.json" | sha256sum -c -
printf '%s  %s\n' "${SPEC168_DIAGNOSTIC_SCRIPT_SHA256#sha256:}" \
  "$SPEC168_DIAGNOSTIC_SCRIPT" | sha256sum -c -

mkdir -p "$SPEC168_OUTPUT_DIR"
chmod 0700 "$SPEC168_OUTPUT_DIR"
diag_dir=$(dirname "$SPEC168_DIAGNOSTIC_SCRIPT")

apptainer exec --nv --cleanenv --containall \
  --bind "$SPEC168_SOURCE_DIR:/source:ro" \
  --bind "$SPEC168_ARTIFACT_DIR:$SPEC168_ARTIFACT_DIR:ro" \
  --bind "$diag_dir:/diag:ro" \
  --bind "$SPEC168_OUTPUT_DIR:/output:rw" \
  --env SPEC168_OVERLAY_ROOT=/output/python-overlay \
  "$SPEC168_RUNTIME_SIF" \
  bash /source/jobs/spec168-overlay-entrypoint.sh \
    /opt/venv/bin/python /diag/$(basename "$SPEC168_DIAGNOSTIC_SCRIPT") \
      --artifact-dir "$SPEC168_ARTIFACT_DIR" \
      --reference "$SPEC168_ARTIFACT_DIR/reference.json" \
      --prompt-id stage-timeout \
      --output /output/result.json \
      --device cuda:0 \
      --steps 5 \
      --tie-epsilon 0.125

python3 - "$SPEC168_OUTPUT_DIR/result.json" <<'PY'
import json
import sys

result = json.load(open(sys.argv[1], encoding="utf-8"))
assert result["status"] == "PASS", result
assert result["classification"] == "NUMERICALLY_EQUIVALENT_DIVERGENCE", result
assert result["firstDivergence"]["tokenIndex"] == 4, result
PY

test ! -e "$SPEC168_OUTPUT_DIR/python-overlay"
chmod -R a-w "$SPEC168_OUTPUT_DIR"
echo "SPEC168_QWEN_TIE_GATE_PASS jobId=${SLURM_JOB_ID} output=$SPEC168_OUTPUT_DIR"
