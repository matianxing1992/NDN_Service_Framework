#!/bin/bash
set -Eeuo pipefail

: "${SLURM_PROCID:?}"
: "${SPEC162_SHARED:?}"
: "${SPEC162_SIF:?}"
: "${SPEC162_PORT:?}"
: "${SPEC162_ARTIFACT_DIR:?}"
: "${SPEC162_POLICY:?}"
: "${SPEC162_SMOKE_CAMPAIGN:?}"
: "${SPEC162_SMOKE_SUBMISSION_ID:?}"

rank=$SLURM_PROCID
if test -n "${SLURM_TMPDIR:-}"; then
  scratch="${SLURM_TMPDIR%/}/ndnsf-di/spec162-smoke-${SLURM_JOB_ID}-rank-${rank}"
else
  scratch="/tmp/${USER}/ndnsf-di/spec162-smoke-${SLURM_JOB_ID}-rank-${rank}"
fi
case "$scratch" in
  /scratch/tma1/ndnsf-di/spec162-smoke-*|/tmp/tma1/ndnsf-di/spec162-smoke-*) ;;
  *)
    if test -z "${SLURM_TMPDIR:-}" ||
       test "$scratch" != \
         "${SLURM_TMPDIR%/}/ndnsf-di/spec162-smoke-${SLURM_JOB_ID}-rank-${rank}"; then
      echo "UNSAFE_RANK_SCRATCH:$scratch" >&2
      exit 4
    fi
    ;;
esac
test ! -e "$scratch"
mkdir -p "$scratch/run" "$scratch/home" "$scratch/log" "$scratch/generated"

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  cp -a "$scratch/log/." "$SPEC162_SHARED/node-${rank}/" 2>/dev/null || true
  find "$scratch" -mindepth 1 -delete 2>/dev/null || true
  rmdir -- "$scratch" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

# ONNX external-data files live in a shared project filesystem.  ORT opens
# hundreds of those files while creating a CUDA session; reading them through
# the shared mount can reduce startup to page-at-a-time I/O.  For the real
# local-artifact campaign, stage only this rank's graph and its referenced
# external tensors on node-local scratch, then pass that immutable copy into
# the container.  The outer artifact hash gate remains authoritative; this is
# a transport/cache step, not a second model artifact.
onnx_stage_runtime_path=""
if test "${SPEC175_LOCAL_ARTIFACTS:-0}" = 1; then
  readonly onnx_stage_source="$SPEC162_ARTIFACT_DIR/qwen-onnx-stage-artifacts"
  readonly onnx_stage_cache="$scratch/onnx-stage-artifacts"
  readonly onnx_stage_graph="stage-${rank}-qwen.onnx"
  test -r "$onnx_stage_source/$onnx_stage_graph"
  mkdir -p "$onnx_stage_cache"
  apptainer exec --cleanenv \
    --bind "$onnx_stage_source:/artifacts:ro" \
    "$SPEC162_SIF" /opt/venv/bin/python - "$onnx_stage_graph" \
    > "$scratch/log/onnx-stage-files.list" <<'PY'
import sys
from pathlib import PurePosixPath

import onnx

graph = f"/artifacts/{sys.argv[1]}"
model = onnx.load(graph, load_external_data=False)
tensors = list(model.graph.initializer)
tensors.extend(model.graph.sparse_initializer)
locations = set()
for tensor in tensors:
    for entry in tensor.external_data:
        if entry.key == "location":
            location = str(entry.value)
            path = PurePosixPath(location)
            if path.is_absolute() or ".." in path.parts:
                raise SystemExit(f"unsafe ONNX external-data path: {location}")
            locations.add(location)
for location in sorted(locations):
    print(location)
PY
  while IFS= read -r external_path; do
    test -n "$external_path"
    case "$external_path" in
      /*|../*|*/../*)
        echo "UNSAFE_ONNX_EXTERNAL_DATA_PATH:$external_path" >&2
        exit 4
        ;;
    esac
    test -r "$onnx_stage_source/$external_path"
  done < "$scratch/log/onnx-stage-files.list"
  # Stream one archive through the shared filesystem instead of issuing one
  # metadata-heavy cp for every external tensor.  The destination is still
  # node-local scratch; this only changes transport and preserves the exact
  # graph/tensor bytes and relative external-data layout.
  tar -C "$onnx_stage_source" --verbatim-files-from --no-recursion \
    -cf - --files-from="$scratch/log/onnx-stage-files.list" \
    "$onnx_stage_graph" |
    tar -C "$onnx_stage_cache" -xf -
  onnx_stage_runtime_path="/scratch/onnx-stage-artifacts/$onnx_stage_graph"
  test -r "$onnx_stage_cache/$onnx_stage_graph"
  printf 'graph=%s externalFiles=%s cache=%s\n' \
    "$onnx_stage_graph" \
    "$(wc -l < "$scratch/log/onnx-stage-files.list")" \
    "$onnx_stage_cache" > "$scratch/log/onnx-stage-cache.log"
fi

mkdir -p "$SPEC162_SHARED/node-${rank}"
hostname > "$SPEC162_SHARED/node-${rank}/hostname.txt"
hostname -I | awk '{print $1}' > "$SPEC162_SHARED/node-${rank}/ipv4.txt"
printf '%s\n' "$SPEC162_PORT" > "$SPEC162_SHARED/node-${rank}/port.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$SPEC162_SHARED/node-${rank}/gpu.csv"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e "s|@@PORT@@|${SPEC162_PORT}|g" \
    "$SPEC162_SHARED/source/jobs/nfd.conf.in" > "$scratch/nfd.conf"

readonly repo_override="$SPEC162_SHARED/source/runtime/py_repoclient"
readonly ndnsf_service_override="$SPEC162_SHARED/source/runtime/ndnsf-service.py"
readonly app_sdk_client_override="$SPEC162_SHARED/source/runtime/app-sdk-client.py"
readonly app_sdk_facades_override="$SPEC162_SHARED/source/runtime/app-sdk-facades.py"
readonly di_placement_override="$SPEC162_SHARED/source/runtime/di-placement.py"
readonly di_sdk_placement_override="$SPEC162_SHARED/source/runtime/sdk-placement.py"
readonly di_provider_override="$SPEC162_SHARED/source/runtime/di-provider.py"
readonly qwen_adapter_placement_override="$SPEC162_SHARED/source/runtime/qwen-adapter-placement.py"
readonly presplit_first_override="$SPEC162_SHARED/source/runtime/presplit-first.py"
readonly campaign_basename="$(basename "$SPEC162_SMOKE_CAMPAIGN")"
readonly container_campaign="/shared/${campaign_basename}"
readonly request_timeout_ms="${SPEC162_REQUEST_TIMEOUT_MS:-3600000}"
case "$request_timeout_ms" in
  ''|*[!0-9]*) echo "SPEC162_REQUEST_TIMEOUT_MS_INVALID" >&2; exit 2 ;;
esac
if test "$request_timeout_ms" -lt 60000 ||
   test "$request_timeout_ms" -gt 3600000; then
  echo "SPEC162_REQUEST_TIMEOUT_MS_OUT_OF_RANGE:$request_timeout_ms" >&2
  exit 2
fi
readonly ack_timeout_ms="${SPEC162_ACK_TIMEOUT_MS:-120000}"
case "$ack_timeout_ms" in
  ''|*[!0-9]*) echo "SPEC162_ACK_TIMEOUT_MS_INVALID" >&2; exit 2 ;;
esac
if test "$ack_timeout_ms" -lt 10000 ||
   test "$ack_timeout_ms" -gt 600000; then
  echo "SPEC162_ACK_TIMEOUT_MS_OUT_OF_RANGE:$ack_timeout_ms" >&2
  exit 2
fi
readonly selection_offer_lease_ms="${SPEC162_SELECTION_OFFER_LEASE_MS:-$request_timeout_ms}"
case "$selection_offer_lease_ms" in
  ''|*[!0-9]*) echo "SPEC162_SELECTION_OFFER_LEASE_MS_INVALID" >&2; exit 2 ;;
esac
if test "$selection_offer_lease_ms" -lt "$request_timeout_ms" ||
   test "$selection_offer_lease_ms" -lt $((ack_timeout_ms + 30000)) ||
   test "$selection_offer_lease_ms" -gt 3600000; then
  echo "SPEC162_SELECTION_OFFER_LEASE_MS_OUT_OF_RANGE:$selection_offer_lease_ms" >&2
  exit 2
fi
readonly face_scheme="${SPEC162_FACE_SCHEME:-tcp4}"
case "$face_scheme" in
  tcp4|udp4) ;;
  *) echo "SPEC162_FACE_SCHEME_INVALID:$face_scheme" >&2; exit 2 ;;
esac
test -r "$SPEC162_SMOKE_CAMPAIGN"
# The campaign is small metadata, unlike the ONNX stage/external-weight
# bundle.  Copy it into the already-bound shared directory so the contained
# user sees the same immutable path on every rank.  The outer smoke wrapper
# may already have materialized that exact path; avoid cp's same-file failure.
campaign_target="$SPEC162_SHARED/$campaign_basename"
if test "$(readlink -f "$SPEC162_SMOKE_CAMPAIGN")" != "$(readlink -f "$campaign_target")"; then
  cp "$SPEC162_SMOKE_CAMPAIGN" "$campaign_target"
fi
test -r "$SPEC162_SHARED/$campaign_basename"
for name in __init__.py artifact_api.py artifact_lifecycle.py \
  artifact_transfer.py network_artifact_backend.py orchestration.py \
  persistence.py service_names.py; do
  test -r "$repo_override/$name"
done
test -r "$ndnsf_service_override"
test -r "$app_sdk_client_override"
test -r "$app_sdk_facades_override"
test -r "$di_placement_override"
test -r "$di_sdk_placement_override"
test -r "$di_provider_override"
test -r "$qwen_adapter_placement_override"
test -r "$presplit_first_override"

# Keep Apptainer's image/session cache on the same node-local scratch that was
# preflighted for this rank.  Without this, a large SIF can fall back to the
# user's project/home cache, making an otherwise valid launch appear hung or
# fail from an unrelated quota/filesystem boundary.
readonly apptainer_tmp="$scratch/apptainer-tmp"
readonly apptainer_cache="$scratch/apptainer-cache"
mkdir -p "$apptainer_tmp" "$apptainer_cache"
export APPTAINER_TMPDIR="$apptainer_tmp"
export APPTAINER_CACHEDIR="$apptainer_cache"

# Keep the SIF-native pybind extension visible.  Overriding the package at
# /opt/ndnsf-app/python shadows site-packages/py_repoclient and makes the
# installed _py_repoclient module undiscoverable.
apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/${USER}" \
  --bind "$scratch:/scratch:rw" \
  --bind "$SPEC162_SHARED:/shared:rw" \
  --bind "$SPEC162_SHARED/source:/source:ro" \
  --bind "$SPEC162_ARTIFACT_DIR:$SPEC162_ARTIFACT_DIR:ro" \
  --bind "$repo_override/__init__.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/__init__.py:ro" \
  --bind "$repo_override/artifact_api.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/artifact_api.py:ro" \
  --bind "$repo_override/artifact_lifecycle.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/artifact_lifecycle.py:ro" \
  --bind "$repo_override/artifact_transfer.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/artifact_transfer.py:ro" \
  --bind "$repo_override/network_artifact_backend.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/network_artifact_backend.py:ro" \
  --bind "$repo_override/orchestration.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/orchestration.py:ro" \
  --bind "$repo_override/persistence.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/persistence.py:ro" \
  --bind "$repo_override/service_names.py:/opt/venv/lib/python3.10/site-packages/py_repoclient/service_names.py:ro" \
  --bind "$ndnsf_service_override:/opt/ndnsf-app/python/ndnsf/service.py:ro" \
  --bind "$app_sdk_client_override:/opt/ndnsf-app/python/ndnsf_distributed_inference/app_sdk/client.py:ro" \
  --bind "$app_sdk_facades_override:/opt/ndnsf-app/python/ndnsf_distributed_inference/app_sdk/facades.py:ro" \
  --bind "$di_placement_override:/opt/ndnsf-app/python/ndnsf_distributed_inference/app_sdk/placement.py:ro" \
  --bind "$di_sdk_placement_override:/opt/ndnsf-app/python/ndnsf_distributed_inference/sdk/placement.py:ro" \
  --bind "$di_provider_override:/opt/ndnsf-app/python/ndnsf_distributed_inference/provider.py:ro" \
  --bind "$qwen_adapter_placement_override:/opt/ndnsf-app/python/ndnsf_distributed_inference/adapters/qwen/placement.py:ro" \
  --bind "$presplit_first_override:/opt/ndnsf-app/python/ndnsf_distributed_inference/planner/presplit_first.py:ro" \
  --env "SPEC162_RANK=${rank}" \
  --env "SPEC162_PORT=${SPEC162_PORT}" \
  --env "SPEC162_POLICY=${SPEC162_POLICY}" \
  --env "SPEC162_SMOKE_CAMPAIGN=${container_campaign}" \
  --env "SPEC162_SMOKE_SUBMISSION_ID=${SPEC162_SMOKE_SUBMISSION_ID}" \
  --env "SPEC162_ARTIFACT_DIR=${SPEC162_ARTIFACT_DIR}" \
  --env "SPEC162_REQUEST_TIMEOUT_MS=${request_timeout_ms}" \
  --env "SPEC162_ACK_TIMEOUT_MS=${ack_timeout_ms}" \
  --env "SPEC162_SELECTION_OFFER_LEASE_MS=${selection_offer_lease_ms}" \
  --env "SPEC162_PROVIDER_READY_WAIT_S=${SPEC162_PROVIDER_READY_WAIT_S:-7200}" \
  --env "SPEC162_FACE_SCHEME=${face_scheme}" \
  --env "CUDA_VISIBLE_DEVICES=${CUDA_VISIBLE_DEVICES:-}" \
  --env "SPEC175_LOCAL_ARTIFACTS=${SPEC175_LOCAL_ARTIFACTS:-0}" \
  --env "SPEC175_LOCAL_STAGE_PATH=${onnx_stage_runtime_path}" \
  --env "NDNSF_PIPELINE_MARKER_LOG=/scratch/log/provider-markers-${rank}.log" \
  --env "NDNSF_QWEN_ORT_PROFILE_DIR=${NDNSF_QWEN_ORT_PROFILE_DIR:-}" \
  --env "NDNSF_ARTIFACT_DEBUG_ERRORS=${NDNSF_ARTIFACT_DEBUG_ERRORS:-0}" \
  "$SPEC162_SIF" bash /source/jobs/generation-rank-inner.sh
