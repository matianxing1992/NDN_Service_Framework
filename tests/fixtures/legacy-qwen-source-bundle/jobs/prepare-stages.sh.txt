#!/bin/bash
set -Eeuo pipefail

: "${SPEC162_PREP_WORK_ROOT:?}"
: "${SPEC162_PREP_SOURCE_ROOT:?}"
: "${SPEC162_PROMOTED_ARTIFACT_DIR:?}"
: "${SPEC162_PREP_SUBMISSION_ID:?}"
: "${SPEC162_CAPACITY_DECISION_SHA256:?}"
: "${SPEC162_SIF_SHA256:?}"
: "${SPEC162_PREP_SOURCE_SHA256:?}"

model_profile="${SPEC162_MODEL_PROFILE:-qwen36-27b}"
case "$model_profile" in
  qwen36-27b|qwen3-0.6b) ;;
  *) echo "SPEC162_MODEL_PROFILE_INVALID:$model_profile" >&2; exit 2 ;;
esac

exec /opt/venv/bin/python \
  /source/jobs/prepare-qwen36.py \
  --work-root /work \
  --prompt-set /source/contracts/prompt-set.json \
  --promoted-artifact-dir "$SPEC162_PROMOTED_ARTIFACT_DIR" \
  --submission-id "$SPEC162_PREP_SUBMISSION_ID" \
  --capacity-decision-sha256 "$SPEC162_CAPACITY_DECISION_SHA256" \
  --runtime-sif-sha256 "$SPEC162_SIF_SHA256" \
  --source-bundle-sha256 "$SPEC162_PREP_SOURCE_SHA256" \
  --model-profile "$model_profile"
