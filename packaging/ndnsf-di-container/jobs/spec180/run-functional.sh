#!/usr/bin/env bash
set -euo pipefail

# Workload execution is deliberately separate from profile rendering.  The
# submit wrapper supplies only the explicit SPEC180_* variables in the fixed
# profile.  This host-side Slurm wrapper enters the sealed SIF; it never runs
# the case runner from the submit directory or from the host Python runtime.
: "${SPEC180_GATE:?SPEC180_GATE is required}"
: "${SPEC180_PROFILE_ID:?SPEC180_PROFILE_ID is required}"
: "${SPEC180_PROFILE_SHA256:?SPEC180_PROFILE_SHA256 is required}"
: "${SPEC180_RUN_ID:?SPEC180_RUN_ID is required}"
: "${SPEC180_CANDIDATE_ID:?SPEC180_CANDIDATE_ID is required}"
: "${SPEC180_CANDIDATE_DIGEST:?SPEC180_CANDIDATE_DIGEST is required}"
: "${SPEC180_SIF:?SPEC180_SIF is required}"
: "${SPEC180_SIF_SHA256:?SPEC180_SIF_SHA256 is required}"
: "${SPEC180_MODEL_MANIFEST:?SPEC180_MODEL_MANIFEST is required}"
: "${SPEC180_MODEL_MANIFEST_SHA256:?SPEC180_MODEL_MANIFEST_SHA256 is required}"
: "${SPEC180_MODEL_ROOT:?SPEC180_MODEL_ROOT is required}"
: "${SPEC180_WORKLOAD:?SPEC180_WORKLOAD is required}"
: "${SPEC180_WORKLOAD_SHA256:?SPEC180_WORKLOAD_SHA256 is required}"
: "${SPEC180_OUTPUT_ROOT:?SPEC180_OUTPUT_ROOT is required}"

case "$SPEC180_GATE" in
  yolo-functional) ;;
  *) echo "SPEC180_UNKNOWN_GATE:$SPEC180_GATE" >&2; exit 2 ;;
esac

# The profile owns the Apptainer version, not the host path: the local build
# host installs it under /opt while the Tiger compute nodes ship 1.5.3 at
# /usr/bin/apptainer.  Accept only those two known locations.
APPTAINER="/opt/apptainer/1.5.3/bin/apptainer"
if [[ ! -x "$APPTAINER" ]]; then
  APPTAINER="$(command -v apptainer || true)"
fi
[[ -x "$APPTAINER" ]] || { echo "SPEC180_APPTAINER_MISSING" >&2; exit 78; }
[[ "$($APPTAINER version 2>/dev/null | head -1)" == 1.5.3* ]] || {
  echo "SPEC180_APPTAINER_VERSION_MISMATCH" >&2
  exit 78
}
[[ -f "$SPEC180_SIF" ]] || { echo "SPEC180_SIF_MISSING" >&2; exit 78; }
actual_sif_sha256=$(sha256sum "$SPEC180_SIF" | awk '{print $1}')
[[ "$actual_sif_sha256" == "${SPEC180_SIF_SHA256#sha256:}" ]] || {
  echo "SPEC180_SIF_DIGEST_MISMATCH" >&2
  exit 78
}
[[ -f "$SPEC180_MODEL_MANIFEST" ]] || {
  echo "SPEC180_MODEL_MANIFEST_MISSING" >&2
  exit 78
}
[[ "$SPEC180_MODEL_MANIFEST_SHA256" =~ ^sha256:[0-9a-fA-F]{64}$ ]] || {
  echo "SPEC180_MODEL_MANIFEST_DIGEST_INVALID" >&2
  exit 78
}
actual_model_manifest_sha256=$(sha256sum "$SPEC180_MODEL_MANIFEST" | awk '{print $1}')
[[ "$actual_model_manifest_sha256" == "${SPEC180_MODEL_MANIFEST_SHA256#sha256:}" ]] || {
  echo "SPEC180_MODEL_MANIFEST_DIGEST_MISMATCH" >&2
  exit 78
}
[[ -d "$SPEC180_MODEL_ROOT" ]] || {
  echo "SPEC180_MODEL_ROOT_MISSING" >&2
  exit 78
}
[[ -f "$SPEC180_WORKLOAD" ]] || {
  echo "SPEC180_WORKLOAD_MISSING" >&2
  exit 78
}
[[ "$SPEC180_WORKLOAD_SHA256" =~ ^sha256:[0-9a-fA-F]{64}$ ]] || {
  echo "SPEC180_WORKLOAD_DIGEST_INVALID" >&2
  exit 78
}
actual_workload_sha256=$(sha256sum "$SPEC180_WORKLOAD" | awk '{print $1}')
[[ "$actual_workload_sha256" == "${SPEC180_WORKLOAD_SHA256#sha256:}" ]] || {
  echo "SPEC180_WORKLOAD_DIGEST_MISMATCH" >&2
  exit 78
}
mkdir -p "$SPEC180_OUTPUT_ROOT"

# T018 owns the real Tiger node-local protocol execution: the in-image
# launcher renders the candidate-bound argument files from the signed
# workload document, starts one node-local NFD, runs the Controller,
# Repository, four Providers, and one cold User request, and verifies the
# terminal oracle.  The dispatcher entrypoint is probed first so a missing
# launcher chain fails closed before any process starts.
SEALED_JOB_DIR="/opt/ndnsf-di/replay/repo/packaging/ndnsf-di-container/jobs/spec180"
LAUNCHER="$SEALED_JOB_DIR/run-ndnsf-yolo.sh"
RENDERER="$SEALED_JOB_DIR/render-tiger-yb-args.py"
if ! "$APPTAINER" exec --cleanenv "$SPEC180_SIF" /bin/bash -c '
  test -x "$1/run-ndnsf-yolo.sh" &&
  test -r "$1/render-tiger-yb-args.py" &&
  test -r "$1/supervise-tiger.py" &&
  test -r "$1/bootstrap-tiger-identities.sh" &&
  test -r /opt/ndnsf-di/replay/repo/scripts/validate_spec180_results.py
' bash "$SEALED_JOB_DIR"; then
  echo "SPEC180_CASE_LAUNCHER_NOT_READY" >&2
  exit 78
fi
exec "$APPTAINER" exec --nv --cleanenv --pwd /bundle \
  --bind "${SPEC180_BUNDLE_DIR:-$SLURM_SUBMIT_DIR}:/bundle:ro" \
  --bind "$SPEC180_SIF:/inputs/candidate.sif:ro" \
  --bind "$SPEC180_MODEL_MANIFEST:/inputs/model-manifest.json:ro" \
  --bind "$SPEC180_MODEL_ROOT:/models:ro" \
  --bind "$SPEC180_WORKLOAD:/inputs/workload.json:ro" \
  --bind "$SPEC180_OUTPUT_ROOT:/evidence:rw" \
  --env "SPEC180_GATE=$SPEC180_GATE" \
  --env "SPEC180_PROFILE_ID=$SPEC180_PROFILE_ID" \
  --env "SPEC180_PROFILE_SHA256=$SPEC180_PROFILE_SHA256" \
  --env "SPEC180_RUN_ID=$SPEC180_RUN_ID" \
  --env "SPEC180_CANDIDATE_ID=$SPEC180_CANDIDATE_ID" \
  --env "SPEC180_CANDIDATE_DIGEST=$SPEC180_CANDIDATE_DIGEST" \
  --env "SPEC180_OUTPUT_ROOT=/evidence" \
  --env "SLURM_JOB_ID=${SLURM_JOB_ID:-}" \
  "$SPEC180_SIF" /bin/bash -lc '
    set -euo pipefail
    test -f '"$RENDERER"' || { echo SPEC180_RENDERER_MISSING >&2; exit 78; }
    mkdir -p /evidence/args
    /opt/venv/bin/python '"$RENDERER"' \
      --workload /inputs/workload.json \
      --output-dir /evidence/args \
      --model-root /models
    '"$LAUNCHER"' \
      --scratch /evidence/runtime \
      --evidence /evidence \
      --nfd-config /evidence/args/nfd.conf \
      --controller-args /evidence/args/controller.args \
      --repo-args /evidence/args/repo.args \
      --provider-args-dir /evidence/args/providers \
      --user-args /evidence/args/user.args
  '
