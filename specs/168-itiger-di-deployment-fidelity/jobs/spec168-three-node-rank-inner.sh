#!/bin/bash
set -Eeuo pipefail

# Spec 168 deliberately reuses the measured Spec 162 process/route bootstrap
# as a compatibility kernel.  This adapter is the only translation boundary:
# the public launch surface, immutable bindings, source overlay, request ID,
# and post-run acceptance contract are all owned by Spec 168.
: "${SPEC168_RANK:?}"
: "${SPEC168_PORT:?}"
: "${SPEC168_POLICY:?}"
: "${SPEC168_REQUEST_ID:?}"
: "${SPEC168_REQUEST_TIMEOUT_MS:?}"
: "${SPEC168_ACK_TIMEOUT_MS:?}"
: "${SPEC168_SELECTION_OFFER_LEASE_MS:?}"
control_plane_canary="${SPEC168_CONTROL_PLANE_CANARY:-0}"
case "$control_plane_canary" in
  0)
    : "${SPEC168_GENERATION_CAMPAIGN:?}"
    : "${SPEC168_ARTIFACT_DIR:?}"
    : "${SPEC168_MODEL_IDENTITY_DIGEST:?}"
    : "${SPEC168_WORKLOAD_DIGEST:?}"
    ;;
  1) ;;
  *) echo "SPEC168_CONTROL_PLANE_CANARY_INVALID:$control_plane_canary" >&2; exit 2 ;;
esac

for forbidden in SPEC162_PROVIDER_SETTLE_SECONDS SPEC162_USER_STARTUP_SETTLE_MS \
  SPEC168_PROVIDER_SETTLE_SECONDS SPEC168_USER_STARTUP_SETTLE_MS; do
  if test -n "${!forbidden:-}"; then
    echo "SPEC168_FIXED_SETTLE_FORBIDDEN:${forbidden}" >&2
    exit 2
  fi
done

case "$SPEC168_REQUEST_ID" in
  ""|*[!A-Za-z0-9._-]*)
    echo "SPEC168_REQUEST_ID_INVALID:${SPEC168_REQUEST_ID}" >&2
    exit 2
    ;;
esac

# The formal rank path must use the same complete overlay entrypoint as Gate C.
# Job 182511 bypassed that entrypoint and therefore ran the old SIF's compiled
# collaboration logic even though the immutable source bundle contained the
# provider-projection native closure.  Requiring the fan-out ABI below prevents
# a source/SIF behavior split from reaching another model campaign.
export SPEC168_OVERLAY_ROOT=/scratch/python-overlay
export SPEC168_REQUIRE_SELECTION_FANOUT_ABI=1

export SPEC162_RANK="$SPEC168_RANK"
export SPEC162_PORT="$SPEC168_PORT"
export SPEC162_POLICY="$SPEC168_POLICY"
export SPEC162_SMOKE_SUBMISSION_ID="$SPEC168_REQUEST_ID"
export SPEC162_REQUEST_TIMEOUT_MS="$SPEC168_REQUEST_TIMEOUT_MS"
export SPEC162_ACK_TIMEOUT_MS="$SPEC168_ACK_TIMEOUT_MS"
export SPEC162_SELECTION_OFFER_LEASE_MS="$SPEC168_SELECTION_OFFER_LEASE_MS"
export SPEC162_FACE_SCHEME="${SPEC168_FACE_SCHEME:-tcp4}"
export SPEC162_CONTROL_PLANE_CANARY="$control_plane_canary"
if test "$control_plane_canary" = 0; then
  export SPEC162_SMOKE_CAMPAIGN="$SPEC168_GENERATION_CAMPAIGN"
  export SPEC162_ARTIFACT_DIR="$SPEC168_ARTIFACT_DIR"
  export SPEC162_MODEL_IDENTITY_DIGEST="$SPEC168_MODEL_IDENTITY_DIGEST"
  export SPEC162_WORKLOAD_DIGEST="$SPEC168_WORKLOAD_DIGEST"
fi

printf 'SPEC168_COMPAT_KERNEL_ENTER requestId=%s kernel=spec162-measured-bootstrap mode=REQUEST_FIRST\n' \
  "$SPEC168_REQUEST_ID"
exec bash /source/jobs/spec168-overlay-entrypoint.sh \
  bash /source/compat/spec162/generation-rank-inner.sh
