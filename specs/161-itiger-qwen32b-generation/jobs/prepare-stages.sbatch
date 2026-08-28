#!/bin/bash
# Invoked with srun inside prepare-reference.sbatch so the downloaded source,
# H100 reference, and stage construction share one allocation-scoped scratch.
set -Eeuo pipefail

: "${SPEC161_PREP_WORK_ROOT:?}"
: "${SPEC161_PREP_SOURCE_ROOT:?}"
: "${SPEC161_PROMOTED_ARTIFACT_DIR:?}"
: "${SPEC161_PREP_SUBMISSION_ID:?}"
: "${SPEC161_CAPACITY_DECISION_SHA256:?}"
: "${SPEC161_SIF_SHA256:?}"
: "${SPEC161_PREP_SOURCE_SHA256:?}"

exec /opt/venv/bin/python \
  /source/jobs/prepare-qwen32b.py \
  --work-root /work \
  --prompt-set /source/contracts/prompt-set.json \
  --promoted-artifact-dir "$SPEC161_PROMOTED_ARTIFACT_DIR" \
  --submission-id "$SPEC161_PREP_SUBMISSION_ID" \
  --capacity-decision-sha256 "$SPEC161_CAPACITY_DECISION_SHA256" \
  --runtime-sif-sha256 "$SPEC161_SIF_SHA256" \
  --source-bundle-sha256 "$SPEC161_PREP_SOURCE_SHA256"
