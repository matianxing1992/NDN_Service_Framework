#!/usr/bin/env bash
#SBATCH --job-name=spec168-gate-c
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:20:00
#SBATCH --output=/project/tma1/ndnsf-di/slurm/spec168-gate-c-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/slurm/spec168-gate-c-%j.err

set -euo pipefail

: "${SPEC168_SOURCE_DIR:?}"
: "${SPEC168_SOURCE_DIGEST:?}"
: "${SPEC168_SOURCE_BUNDLE_DIGEST:?}"
: "${SPEC168_RUNTIME_SIF:?}"
: "${SPEC168_RUNTIME_SIF_SHA256:?}"
: "${SPEC168_ARTIFACT_DIR:?}"
: "${SPEC168_STAGE_MANIFEST_SHA256:?}"
: "${SPEC168_MODEL_DIGEST:?}"
: "${SPEC168_MODEL_REVISION:?}"
: "${SPEC168_OUTPUT_DIR:?}"

test -d "$SPEC168_SOURCE_DIR"
test -r "$SPEC168_SOURCE_DIR/source-files.sha256"
test -r "$SPEC168_RUNTIME_SIF"
test -r "$SPEC168_ARTIFACT_DIR/stage-manifest.json"
test -r "$SPEC168_ARTIFACT_DIR/reference.json"
test ! -e "$SPEC168_OUTPUT_DIR"

observed_source="sha256:$(sha256sum "$SPEC168_SOURCE_DIR/source-files.sha256" | cut -d' ' -f1)"
test "$observed_source" = "$SPEC168_SOURCE_BUNDLE_DIGEST"
python3 "$SPEC168_SOURCE_DIR/jobs/spec168_verify_source_bundle.py" \
  --root "$SPEC168_SOURCE_DIR" \
  --expected-manifest-digest "$SPEC168_SOURCE_BUNDLE_DIGEST"
observed_sif="$(sha256sum "$SPEC168_RUNTIME_SIF" | cut -d' ' -f1)"
test "$observed_sif" = "$SPEC168_RUNTIME_SIF_SHA256"
observed_manifest="$(sha256sum "$SPEC168_ARTIFACT_DIR/stage-manifest.json" | cut -d' ' -f1)"
test "$observed_manifest" = "$SPEC168_STAGE_MANIFEST_SHA256"

mkdir -p "$SPEC168_OUTPUT_DIR"
chmod 0700 "$SPEC168_OUTPUT_DIR"

apptainer exec --nv --cleanenv --containall \
  --bind "$SPEC168_SOURCE_DIR:/source:ro" \
  --bind "$SPEC168_ARTIFACT_DIR:$SPEC168_ARTIFACT_DIR:ro" \
  --bind "$SPEC168_OUTPUT_DIR:/output:rw" \
  --env "SPEC168_OVERLAY_ROOT=/output/python-overlay" \
  --env "SPEC168_RUNTIME_SIF_SHA256=sha256:$observed_sif" \
  --env "SPEC168_SOURCE_DIGEST=$SPEC168_SOURCE_DIGEST" \
  --env "SPEC168_SOURCE_BUNDLE_DIGEST=$SPEC168_SOURCE_BUNDLE_DIGEST" \
  "$SPEC168_RUNTIME_SIF" \
  bash /source/jobs/spec168-overlay-entrypoint.sh \
    /opt/venv/bin/python /source/jobs/spec168_exact_sif_cuda_preflight.py \
    --stage-manifest "$SPEC168_ARTIFACT_DIR/stage-manifest.json" \
    --reference "$SPEC168_ARTIFACT_DIR/reference.json" \
    --output /output/preflight-result.json \
    --expected-stage-manifest-digest "sha256:$observed_manifest" \
    --expected-model-digest "$SPEC168_MODEL_DIGEST" \
    --expected-revision "$SPEC168_MODEL_REVISION" \
    --expected-sif-digest "sha256:$observed_sif" \
    --expected-source-digest "$SPEC168_SOURCE_DIGEST" \
    --expected-source-bundle-digest "$SPEC168_SOURCE_BUNDLE_DIGEST"

test "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["status"])' "$SPEC168_OUTPUT_DIR/preflight-result.json")" = PASS
test ! -e "$SPEC168_OUTPUT_DIR/python-overlay"
chmod -R a-w "$SPEC168_OUTPUT_DIR"
echo "SPEC168_GATE_C_JOB_PASS jobId=${SLURM_JOB_ID} output=$SPEC168_OUTPUT_DIR"
