#!/usr/bin/env bash
#SBATCH --job-name=ndnsf175-stage
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
# One GPU owns each complete pipeline stage.  Loading all three 27B stages on
# one GPU is not a valid readiness test and can exhaust the CUDA arena.
# Qwen3.6-27B stage artifacts require the RTX 6000 capacity used by the
# validated T025 job; RTX 5000 is not an interchangeable target here.
#SBATCH --gres=gpu:rtx_6000:3
#SBATCH --time=00:45:00
#SBATCH --output=%x-%j.out
#SBATCH --error=%x-%j.err
set -euo pipefail
export SPEC175_GATE=stage-readiness
SPEC175_JOB_ROOT="${SPEC175_JOB_ROOT:-${SLURM_SUBMIT_DIR:-$PWD}/packaging/ndnsf-di-container/jobs/spec175}"
export SPEC175_SIF="$SPEC175_LOCAL_SIF"
export SPEC175_SIF_SHA256="$SPEC175_LOCAL_SIF_SHA256"
export SPEC175_BUNDLE="${SPEC175_BUNDLE:?set SPEC175_BUNDLE to the staged bundle}"
export SPEC175_OUTPUT="${SPEC175_OUTPUT:-$PWD/spec175-stage-$SLURM_JOB_ID}"
: "${SPEC175_MODEL_MANIFEST:?set SPEC175_MODEL_MANIFEST to the frozen stage manifest}"
: "${SPEC175_REMOTE_MODEL_ROOT:?set SPEC175_REMOTE_MODEL_ROOT to the external model root}"

runner="$SPEC175_JOB_ROOT/run-qwen-stage-readiness.py"
repo_root=$(cd "$SPEC175_JOB_ROOT/../../../.." && pwd)
oracle_runner="$repo_root/specs/175-ndnsf-di-streamed-invocation/jobs/generate-qwen-onnx-reference.py"
workload="$SPEC175_JOB_ROOT/workload.json"
[[ -f "$runner" ]] || { echo SPEC175_STAGE_RUNNER_MISSING >&2; exit 4; }
[[ -f "$oracle_runner" ]] || { echo SPEC175_ORACLE_RUNNER_MISSING >&2; exit 4; }
[[ -f "$workload" ]] || { echo SPEC175_WORKLOAD_MISSING >&2; exit 4; }
[[ -f "$SPEC175_MODEL_MANIFEST" ]] || {
  echo SPEC175_MODEL_MANIFEST_MISSING >&2
  exit 4
}
[[ -d "$SPEC175_REMOTE_MODEL_ROOT" ]] || {
  echo SPEC175_REMOTE_MODEL_ROOT_MISSING >&2
  exit 4
}
# The profile transports device IDs with ':' because Slurm's --export list is
# comma-delimited.  Decode only after the profile and bundle have passed the
# local semantic checks.
stage_device_ids="${SPEC175_STAGE_DEVICE_IDS:-0:1:2}"
stage_device_ids="${stage_device_ids//:/,}"

actual=$(sha256sum "$SPEC175_SIF" | awk '{print $1}')
[[ "$actual" == "${SPEC175_SIF_SHA256#sha256:}" ]] || {
  echo SPEC175_SIF_DIGEST_MISMATCH >&2
  exit 4
}
python3 - "$SPEC175_MODEL_MANIFEST" "sha256:$actual" <<'PY'
import json
import sys

manifest = json.loads(open(sys.argv[1], encoding="utf-8").read())
if manifest.get("runtimeSifSha256") != sys.argv[2]:
    raise SystemExit("SPEC175_MODEL_SIF_BINDING_MISMATCH")
PY
mkdir -p "$SPEC175_OUTPUT"
cache_root="${SLURM_TMPDIR:-/tmp/$USER/ndnsf-di/$SLURM_JOB_ID}/model-cache"
mkdir -p "$cache_root"

# The verifier is a deployment test helper, not an application/source
# overlay: the candidate SIF still supplies Python 3.10, NumPy, and ORT.  The
# model remains external and read-only; only its content-addressed copy is
# consumed by the measured stage control.
set +e
apptainer exec --nv --cleanenv \
  --bind "$SPEC175_REMOTE_MODEL_ROOT:/model:ro" \
  --bind "$SPEC175_MODEL_MANIFEST:/manifest.json:ro" \
  --bind "$runner:/verify/run-qwen-stage-readiness.py:ro" \
  --bind "$SPEC175_OUTPUT:/evidence:rw" \
  --bind "$cache_root:/cache:rw" \
  "$SPEC175_SIF" python3 /verify/run-qwen-stage-readiness.py \
    --manifest /manifest.json \
    --model-root /model \
    --cache-root /cache \
    --output /evidence/tiger-stage-readiness.json \
    --stage-device-ids "$stage_device_ids" \
    --max-new-tokens 8
status=$?
set -e
if [[ "$status" -eq 0 ]]; then
  set +e
  apptainer exec --nv --cleanenv \
    --bind "$SPEC175_REMOTE_MODEL_ROOT:/model:ro" \
    --bind "$SPEC175_MODEL_MANIFEST:/manifest.json:ro" \
    --bind "$oracle_runner:/verify/generate-qwen-onnx-reference.py:ro" \
    --bind "$workload:/workload.json:ro" \
    --bind "$SPEC175_OUTPUT:/evidence:rw" \
    "$SPEC175_SIF" python3 /verify/generate-qwen-onnx-reference.py \
      --service-manifest /manifest.json \
      --artifact-root /model \
      --workload /workload.json \
      --tokenizer-dir /model/qwen-onnx-tokenizer \
      --output /evidence/g5-oracle.json \
      --stage-device-ids "$stage_device_ids" \
      --max-new-tokens 64 \
      --execution-provider cuda
  status=$?
  set -e
fi
printf '{"schemaVersion":"spec175-stage-readiness-v1","gate":"stage-readiness","exitCode":%d,"status":"%s"}\n' \
  "$status" "$([[ $status -eq 0 ]] && echo PASS || echo FAIL)" \
  >"$SPEC175_OUTPUT/spec175-gate-terminal.json"
exit "$status"
