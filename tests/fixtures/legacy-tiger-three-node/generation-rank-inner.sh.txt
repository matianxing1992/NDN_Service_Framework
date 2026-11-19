#!/bin/bash
set -Eeuo pipefail
# The runtime user emits SPEC162_REQUEST_GATE_OPEN after the secured request
# gate; keep this launcher contract marker adjacent to the rank entrypoint.
# The linked smoke profile is mode=REQUEST_FIRST (one secured request, then
# the internal token loop), even though the mode is selected by user.py.

: "${SPEC162_RANK:?}"
: "${SPEC162_PORT:?}"
: "${SPEC162_POLICY:?}"
: "${SPEC162_SMOKE_SUBMISSION_ID:?}"
: "${SPEC162_REQUEST_TIMEOUT_MS:?}"

control_plane_canary="${SPEC162_CONTROL_PLANE_CANARY:-0}"
case "$control_plane_canary" in
  0)
    : "${SPEC162_SMOKE_CAMPAIGN:?}"
    : "${SPEC162_ARTIFACT_DIR:?}"
    ;;
  1) ;;
  *) echo "SPEC162_CONTROL_PLANE_CANARY_INVALID:$control_plane_canary" >&2; exit 2 ;;
esac

if test "$control_plane_canary" = 0; then
  model_identity_digest="${SPEC162_MODEL_IDENTITY_DIGEST:-}"
  if test -z "$model_identity_digest"; then
    model_identity_digest=$(/opt/venv/bin/python - \
      "$SPEC162_ARTIFACT_DIR/stage-manifest.json" <<'PY'
import json
import sys
value = str(json.load(open(sys.argv[1], encoding="utf-8"))["modelDigest"])
print(value if value.startswith("sha256:") else "sha256:" + value)
PY
    )
  fi
  workload_digest="${SPEC162_WORKLOAD_DIGEST:-}"
  if test -z "$workload_digest"; then
    workload_digest="sha256:$(sha256sum "$SPEC162_SMOKE_CAMPAIGN" | cut -d' ' -f1)"
  fi
  case "$model_identity_digest:$workload_digest" in
    sha256:????????????????????????????????????????????????????????????????:sha256:????????????????????????????????????????????????????????????????) ;;
    *) echo "SPEC162_RUNTIME_IDENTITY_DIGEST_INVALID" >&2; exit 2 ;;
  esac
fi

case "$SPEC162_REQUEST_TIMEOUT_MS" in
  ''|*[!0-9]*) echo "SPEC162_REQUEST_TIMEOUT_MS_INVALID" >&2; exit 2 ;;
esac
if test "$SPEC162_REQUEST_TIMEOUT_MS" -lt 60000 ||
   test "$SPEC162_REQUEST_TIMEOUT_MS" -gt 3600000; then
  echo "SPEC162_REQUEST_TIMEOUT_MS_OUT_OF_RANGE:$SPEC162_REQUEST_TIMEOUT_MS" >&2
  exit 2
fi
case "${SPEC162_ACK_TIMEOUT_MS:-120000}" in
  ''|*[!0-9]*) echo "SPEC162_ACK_TIMEOUT_MS_INVALID" >&2; exit 2 ;;
esac
ack_timeout_ms="${SPEC162_ACK_TIMEOUT_MS:-120000}"
if test "$ack_timeout_ms" -lt 10000 ||
   test "$ack_timeout_ms" -gt 600000; then
  echo "SPEC162_ACK_TIMEOUT_MS_OUT_OF_RANGE:$ack_timeout_ms" >&2
  exit 2
fi
case "${SPEC162_SELECTION_OFFER_LEASE_MS:-$SPEC162_REQUEST_TIMEOUT_MS}" in
  ''|*[!0-9]*) echo "SPEC162_SELECTION_OFFER_LEASE_MS_INVALID" >&2; exit 2 ;;
esac
selection_offer_lease_ms="${SPEC162_SELECTION_OFFER_LEASE_MS:-$SPEC162_REQUEST_TIMEOUT_MS}"
if test "$selection_offer_lease_ms" -lt "$SPEC162_REQUEST_TIMEOUT_MS" ||
   test "$selection_offer_lease_ms" -lt $((ack_timeout_ms + 30000)) ||
   test "$selection_offer_lease_ms" -gt 3600000; then
  echo "SPEC162_SELECTION_OFFER_LEASE_MS_OUT_OF_RANGE:$selection_offer_lease_ms" >&2
  exit 2
fi

face_scheme="${SPEC162_FACE_SCHEME:-tcp4}"
case "$face_scheme" in
  tcp4|udp4) ;;
  *) echo "SPEC162_FACE_SCHEME_INVALID:$face_scheme" >&2; exit 2 ;;
esac

if test "$control_plane_canary" = 0; then
  selection_model_type=$(/opt/venv/bin/python -c '
import json, sys
profile = str(json.load(open(sys.argv[1], encoding="utf-8")).get("modelProfile", ""))
mapping = {
    "qwen3-0.6b": "qwen3",
    "qwen3.6-27b": "qwen3_5",
    # Spec175 names the deployed canonical artifact explicitly so the
    # selection metadata cannot accidentally launch a Transformers profile.
    "qwen3.6-27b-onnx-cuda": "qwen3_5",
}
if not profile:
    # Older immutable Qwen3.6 stage manifests predate the explicit
    # modelProfile field but bind the same model through repository identity.
    manifest = json.load(open(sys.argv[1], encoding="utf-8"))
    if manifest.get("repository") == "Qwen/Qwen3.6-27B":
        profile = "qwen3.6-27b"
if profile not in mapping:
    raise SystemExit("SPEC162_SELECTION_MODEL_TYPE_UNSUPPORTED:" + profile)
print(mapping[profile])
' "$SPEC162_ARTIFACT_DIR/stage-manifest.json")
fi

rank=$SPEC162_RANK
pids=()

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  if test "$rc" -ne 0; then
    printf 'rank=%s rc=%s\n' "$rank" "$rc" \
      > "/shared/rank-failed-${rank}.txt" 2>/dev/null || true
    touch /shared/rank-abort 2>/dev/null || true
  fi
  # Persist runtime logs before terminating Provider/NFD processes.  This is
  # intentionally done inside the container as well as in the outer rank
  # wrapper: a Slurm cancellation can interrupt the wrapper before its final
  # copy, but per-stage fetch/progress and final-response evidence must remain
  # available for a negative-result audit.
  mkdir -p "/shared/node-${rank}"
  cp -a /scratch/log/. "/shared/node-${rank}/" 2>/dev/null || true
  for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  rm -f \
    "/scratch/provider-bootstrap-${rank}.token" \
    "/scratch/repo-user-bootstrap.token" \
    "/scratch/selection-storage-${rank}.key" \
    "/scratch/selection-offer-${rank}.key" \
    "/shared/selection-offer-${rank}.key"
  nfdc status report > /scratch/log/nfd-status-exit.txt 2>/dev/null || true
  if test -n "${nfd_pid:-}"; then kill "$nfd_pid" 2>/dev/null || true; fi
  wait "${nfd_pid:-}" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_file()
{
  path=$1
  local attempts="${2:-1800}"
  for _ in $(seq 1 "$attempts"); do
    test -f "$path" && return 0
    if test -f /shared/rank-abort; then
      echo "RANK_ABORTED_WHILE_WAITING:${path}" >&2
      return 6
    fi
    sleep 0.1
  done
  echo "BARRIER_TIMEOUT:$path" >&2
  return 5
}

wait_all()
{
  stem=$1
  wait_file "/shared/${stem}-0"
  wait_file "/shared/${stem}-1"
  wait_file "/shared/${stem}-2"
}

bootstrap_token_for_identity()
{
  identity=$1
  token_file=$2
  awk -v id="$identity" '
    $0 !~ /^[[:space:]]*#/ && $1 == id { print $2; found=1; exit }
    END { if (!found) exit 1 }
  ' "$token_file"
}

run_nfdc()
{
  timeout 30s nfdc "$@"
}

create_peer_face()
{
  uri=$1
  log_path=/scratch/log/face-create.txt
  for attempt in $(seq 1 300); do
    if timeout 5s nfdc face create "$uri" persistency persistent \
      >>"$log_path" 2>&1; then
      return 0
    fi
    # A reciprocal face may already exist after the peer initiated the
    # connection.  Treat that state as success, but only after asking NFD
    # for the exact remote URI; this prevents a transient create failure from
    # silently leaving a one-way allocation mesh.
    if timeout 5s nfdc face list remote "$uri" \
      >>"$log_path" 2>&1; then
      return 0
    fi
    printf 'face-create-retry attempt=%s uri=%s\n' "$attempt" "$uri" \
      >>"$log_path"
    sleep 0.1
  done
  echo "PEER_FACE_CREATE_TIMEOUT:$uri" >&2
  return 5
}

nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
nfd_ready=0
for _ in $(seq 1 600); do
  if test -S /scratch/run/nfd.sock &&
     run_nfdc status >/dev/null 2>&1; then
    nfd_ready=1
    break
  fi
  kill -0 "$nfd_pid" 2>/dev/null || exit 5
  sleep 0.1
done
if test "$nfd_ready" -ne 1; then
  echo "NFD_START_TIMEOUT:socket=/scratch/run/nfd.sock:wait_s=60" >&2
  exit 5
fi
touch "/shared/nfd-ready-${rank}"
wait_all nfd-ready

for peer_rank in 0 1 2; do
  test "$peer_rank" -eq "$rank" && continue
  peer_ip=$(</shared/node-${peer_rank}/ipv4.txt)
  peer_port="${SPEC162_PORT}"
  if test -s "/shared/node-${peer_rank}/port.txt"; then
    peer_port=$(</shared/node-${peer_rank}/port.txt)
  fi
  uri="${face_scheme}://${peer_ip}:${peer_port}"
  create_peer_face "$uri"
  run_nfdc route add /NDNSF-DistributeInference/example "$uri" origin 65 cost 0
  # MiniNDN advertises the SVS group identity as its own origin.  Keep the
  # aggregate application route for ordinary traffic, but install the exact
  # group prefix as well so SVS sync Interests do not depend on parent-prefix
  # inheritance in a manually assembled Tiger FIB.
  run_nfdc route add /NDNSF-DistributeInference/example/group "$uri" \
    origin 65 cost 0
  run_nfdc route add /NDNSF/DistributedRepo "$uri" origin 65 cost 0
  run_nfdc route add /activation/llm "$uri" origin 65 cost 0
  case "$peer_rank" in
    0)
      peer_provider_prefix=/NDNSF-DistributeInference/example/provider
      ;;
    1|2)
      peer_provider_prefix=/NDNSF-DistributeInference/example/provider/${peer_rank}
      ;;
    *)
      echo "INVALID_PEER_RANK:$peer_rank" >&2
      exit 2
      ;;
  esac
  # The aggregate multicast route remains useful for SVS discovery, but
  # identity-bound certificate and mapping fetches must have one deterministic
  # owner.  Without these exact routes, a remote Provider can repeatedly
  # validate peer group mappings while never reaching the User's mapping.
  run_nfdc route add "$peer_provider_prefix" "$uri" origin 66 cost 0
done
if test "$rank" -ne 0; then
  root_ip=$(</shared/node-0/ipv4.txt)
  root_uri="${face_scheme}://${root_ip}:${SPEC162_PORT}"
  # Repo receipts name the segmented data under the root Provider identity
  # (``.../provider/NDNSF-ARTIFACT/...``), while the generic application
  # prefix is also advertised by every peer.  Install an exact data-plane
  # route so a remote Provider cannot send a large-artifact fetch into the
  # multicast aggregate and time out during Selection preparation.
  run_nfdc route add \
    /NDNSF-DistributeInference/example/provider/NDNSF-ARTIFACT \
    "$root_uri" origin 68 cost 0
  # Artifact fetches are unicast data-plane traffic.  The aggregate
  # application prefix uses multicast for SVS/control discovery, but leaving
  # this child prefix on multicast floods every large-object Interest to all
  # peers and creates a PIT/unsatisfied-Interest storm under concurrent stage
  # preparation.  Pin the artifact child prefix to best-route.
  run_nfdc strategy set \
    /NDNSF-DistributeInference/example/provider/NDNSF-ARTIFACT \
    /localhost/nfd/strategy/best-route
  run_nfdc route add /NDNSF-DistributeInference/example/user "$root_uri" \
    origin 66 cost 0
  # SVS fetches the user's mapping and published request under the producer
  # namespace.  Keep the aggregate identity route above for ordinary
  # application traffic, but add the producer prefix explicitly so a remote
  # provider never falls back to the multicast aggregate while the first
  # mapping is converging.  This is a routing invariant, not a security
  # bypass: the request and certificate still pass the normal validators.
  run_nfdc route add /NDNSF-DistributeInference/example/user/user/0 \
    "$root_uri" origin 67 cost 0
  run_nfdc route add /NDNSF-DistributeInference/example/controller "$root_uri" \
    origin 66 cost 0
  # DKEY is a controller-owned control prefix.  Install the exact route so
  # remote Providers do not multicast DKEY Interests across the peer mesh.
  run_nfdc route add /NDNSF-DistributeInference/example/controller/DKEY \
    "$root_uri" origin 69 cost 0
fi
# DKEY is a point-to-point controller control flow, not peer discovery.
# Apply the child strategy on every rank (including the root rank) so the
# inherited multicast strategy cannot re-fan these Interests after the exact
# route is installed.
run_nfdc strategy set /NDNSF-DistributeInference/example/controller/DKEY \
  /localhost/nfd/strategy/best-route
run_nfdc strategy set /NDNSF-DistributeInference/example \
  /localhost/nfd/strategy/multicast
run_nfdc strategy set /NDNSF-DistributeInference/example/group \
  /localhost/nfd/strategy/multicast
run_nfdc strategy set /NDNSF/DistributedRepo \
  /localhost/nfd/strategy/multicast
run_nfdc strategy set /activation/llm /localhost/nfd/strategy/multicast
run_nfdc face list > /scratch/log/face-list-after-routes.txt
run_nfdc route list > /scratch/log/route-list-after-routes.txt
touch "/shared/routes-ready-${rank}"
wait_all routes-ready

# Preserve a candidate-native core already installed by the Spec 168 overlay.
# Putting the SIF library first makes a later Python process load the old core
# beside the new _ndnsf extension, even though the entrypoint ABI probe passed.
export LD_LIBRARY_PATH="/opt/ndn-base/lib:${LD_LIBRARY_PATH:-/opt/ndnsf-app/lib}"
if test "${SPEC168_REQUIRE_SELECTION_FANOUT_ABI:-0}" = 1; then
  /opt/venv/bin/python /source/jobs/spec168-compat-child-abi-check.py
fi
# A Spec 168 caller may provide a complete copy-up overlay that preserves the
# SIF's native extensions while replacing its Python package sources.  Keep
# that caller-supplied path ahead of the installed package; otherwise helper
# programs silently import the stale SIF package even though the entrypoint's
# overlay preflight passed.
export PYTHONPATH="/source/llm_pipeline:/opt/ndnsf-app/python:/opt/venv/lib/python3.10/site-packages:${PYTHONPATH:-}"
export HF_HOME=/scratch/hf-home
export TRANSFORMERS_OFFLINE=1
export HF_HUB_OFFLINE=1
export PYTHONUNBUFFERED=1
export NDN_LOG="${NDN_LOG:-ndn_service_framework.*=DEBUG:ndnsf.*=DEBUG}"
export NDNSF_COLLAB_LARGE_FETCH_TIMING=1
export NDNSF_TIMELINE_TRACE_SAMPLE_RATE=0.1
# Keep the production SVS period untouched.  ACK collection is the only
# request-level readiness signal; there is no User startup settle sleep.
export NDNSF_SVS_PERIODIC_SYNC_MS="${SPEC162_SVS_PERIODIC_SYNC_MS:-1000}"

readonly frozen_policy="$SPEC162_POLICY"
if test "$rank" -eq 0; then
  /opt/venv/bin/python /source/jobs/build-generation-policy.py \
    --input "$frozen_policy" \
    --output /shared/policy.yaml \
    --provenance /shared/policy-provenance.json \
    --user /NDNSF-DistributeInference/example/user \
    --provider-prefix /NDNSF-DistributeInference/example/provider \
    --repo-provider-prefix /NDNSF-DistributeInference/example/repo \
    > /scratch/log/policy-build.log 2>&1
  touch /shared/policy-ready
else
  wait_file /shared/policy-ready 1800
fi
wait_file /shared/policy.yaml 1800
wait_file /shared/policy-provenance.json 1800
export SPEC162_POLICY=/shared/policy.yaml

if test "$rank" -eq 0; then
  /opt/venv/bin/python - <<'PY' > /scratch/log/controller.log 2>&1 &
from ndnsf_distributed_inference.app_sdk.controller import APPController
import os
import subprocess

controller = APPController.from_config(
    os.environ["SPEC162_POLICY"],
    generated_policy_dir="/scratch/generated/controller",
    bootstrap_token_file="/shared/bootstrap-tokens.txt",
)
with open("/shared/controller.cert", "wb") as cert_out:
    subprocess.run(
        [
            "ndnsec",
            "cert-dump",
            "-i",
            "/NDNSF-DistributeInference/example/controller",
        ],
        stdout=cert_out,
        check=True,
    )
print("SPEC162_CONTROLLER_READY", flush=True)
controller.run()
PY
  controller_pid=$!
  pids+=("$controller_pid")
  for _ in $(seq 1 240); do
    grep -q SPEC162_CONTROLLER_READY /scratch/log/controller.log && break
    kill -0 "$controller_pid" 2>/dev/null || exit 5
    sleep 0.5
  done
  grep -q SPEC162_CONTROLLER_READY /scratch/log/controller.log
  touch /shared/controller-started
else
  wait_file /shared/controller-started
fi

case "$rank" in
  0)
    provider_id=""
    provider_identity="/NDNSF-DistributeInference/example/provider"
    repo_provider_identity="/NDNSF-DistributeInference/example/repo"
    role="/LLM/Pipeline/Stage/0"
    ;;
  1)
    provider_id="1"
    provider_identity="/NDNSF-DistributeInference/example/provider/1"
    repo_provider_identity="/NDNSF-DistributeInference/example/repo/1"
    role="/LLM/Pipeline/Stage/1"
    ;;
  2)
    provider_id="2"
    provider_identity="/NDNSF-DistributeInference/example/provider/2"
    repo_provider_identity="/NDNSF-DistributeInference/example/repo/2"
    role="/LLM/Pipeline/Stage/2"
    ;;
esac
wait_file /shared/bootstrap-tokens.txt 600
wait_file /shared/controller.cert 600
export NDNSF_CONTROLLER_CERT_FILE=/shared/controller.cert
bootstrap_token_for_identity \
  "$repo_provider_identity" /shared/bootstrap-tokens.txt \
  > "/scratch/repo-provider-bootstrap-${rank}.token"
chmod 600 "/scratch/repo-provider-bootstrap-${rank}.token"
bootstrap_token_for_identity \
  "$provider_identity" /shared/bootstrap-tokens.txt \
  > "/scratch/provider-bootstrap-${rank}.token"
chmod 600 "/scratch/provider-bootstrap-${rank}.token"

if test "$control_plane_canary" = 0; then
  repo_free_bytes=$(df -B1 --output=avail /scratch | tail -n 1 | tr -d ' ')
  /opt/venv/bin/python /source/jobs/run-repo-node.py \
  --config "$SPEC162_POLICY" \
  --generated-policy-dir "/scratch/generated/repo-${rank}" \
  --provider-id "$provider_id" \
  --provider-prefix /NDNSF-DistributeInference/example/repo \
  --repo-node "/NDNSF/DistributedRepo/Node/${rank}" \
  --storage-dir "/scratch/repo-${rank}" \
  --state-root "/scratch/operator-state" \
  --free-bytes "$repo_free_bytes" \
  --bootstrap-token-file "/scratch/repo-provider-bootstrap-${rank}.token" \
  > "/scratch/log/repo-${rank}.log" 2>&1 &
repo_pid=$!
pids+=("$repo_pid")
for _ in $(seq 1 1200); do
  grep -q SPEC162_REPO_NODE_STARTING "/scratch/log/repo-${rank}.log" &&
    break
  kill -0 "$repo_pid" 2>/dev/null || exit 5
  sleep 0.5
done
grep -q SPEC162_REPO_NODE_STARTING "/scratch/log/repo-${rank}.log"
rm -f "/scratch/repo-provider-bootstrap-${rank}.token"
touch "/shared/repo-started-${rank}"
wait_all repo-started

head -c 32 /dev/urandom > "/scratch/selection-storage-${rank}.key"
head -c 32 /dev/urandom > "/scratch/selection-offer-${rank}.key"
cp "/scratch/selection-offer-${rank}.key" \
  "/shared/selection-offer-${rank}.key"
touch "/shared/selection-key-ready-${rank}"
wait_all selection-key-ready

if test "$rank" -eq 0; then
  bootstrap_token_for_identity \
    /NDNSF-DistributeInference/example/user \
    /shared/bootstrap-tokens.txt > /scratch/repo-user-bootstrap.token
  chmod 600 /scratch/repo-user-bootstrap.token
  # Keep the bootstrap credential for the User's post-ACK publication.  For
  # the local ONNX qualification path, bind the planner to the exact sealed
  # stage files without copying their bytes into DistributedRepo/project
  # storage.  This remains the normal immutable registration shape.
  /opt/venv/bin/python - <<'PY'
import hashlib
import json
import os
from pathlib import Path

stage_manifest_path = (
    Path(os.environ["SPEC162_ARTIFACT_DIR"]) / "stage-manifest.json")
stage_manifest = json.loads(
    stage_manifest_path.read_text(encoding="utf-8"))
registration = {
    "schemaVersion": "ndnsf-di-qwen36-repo-registration-v1",
    "stageManifestSha256": "sha256:" + hashlib.sha256(
        stage_manifest_path.read_bytes()).hexdigest(),
    "modelDigest": str(stage_manifest["modelDigest"]),
    "revision": str(stage_manifest["revision"]),
    "publisher": "/ndnsf-di/local-artifact-qualification",
    "repositoryReadiness": "LOCAL_CONTENT_ADDRESSED_STAGE_BINDING",
    "completedAtUnixMs": 0,
    "artifacts": [],
}
for stage in stage_manifest["stages"]:
    digest = str(stage["sha256"])
    if not digest.startswith("sha256:"):
        digest = "sha256:" + digest
    registration["artifacts"].append({
        "role": str(stage["role"]),
        "fileSha256": digest,
        "fileBytes": int(stage["bytes"]),
        "objectName": "/NDNSF-DistributeInference/local/ONNX/" + digest[7:],
    })
Path("/shared/repo-registration.json").write_text(
    json.dumps(registration, sort_keys=True, indent=2) + "\n",
    encoding="utf-8")
PY
  /opt/venv/bin/python /source/jobs/build-automatic-planning-manifest.py \
    --stage-manifest "$SPEC162_ARTIFACT_DIR/stage-manifest.json" \
    --repository-prefix \
      "/NDNSF-DistributeInference/example/user/NDNSF-DISTRIBUTED-REPO/OBJECT/QWEN36" \
    --repo-registration /shared/repo-registration.json \
    --output /shared/automatic-planning.json \
    > /scratch/log/automatic-planning.log 2>&1
  /opt/venv/bin/python - <<'PY'
import json
import os
from pathlib import Path

stage_manifest = json.loads((
    Path(os.environ["SPEC162_ARTIFACT_DIR"]) / "stage-manifest.json"
).read_text(encoding="utf-8"))
providers = [
    "/NDNSF-DistributeInference/example/provider",
    "/NDNSF-DistributeInference/example/provider/1",
    "/NDNSF-DistributeInference/example/provider/2",
]
key_map = {
    provider: f"/shared/selection-offer-{index}.key"
    for index, provider in enumerate(providers)
}
Path("/shared/selection-offer-key-map.json").write_text(
    json.dumps(key_map, sort_keys=True) + "\n", encoding="utf-8")
manifest = json.loads(
    Path("/shared/automatic-planning.json").read_text(encoding="utf-8"))
for index, stage in enumerate(manifest["stages"]):
    residency = {
        "role": stage["role"],
        "artifact_digest": stage["sha256"],
        "model_content_digest": manifest["model"]["contentDigest"],
        "semantics_digest": manifest["model"]["semanticsDigest"],
        "graph_digest": manifest["graphDigest"],
        "partition_digest": manifest["candidateDigest"],
        "adapter_id": manifest["adapterId"],
        "adapter_version": manifest["adapterVersion"],
        "precision": str(stage_manifest["dtype"]),
        "backend": "onnxruntime",
    }
    Path(f"/shared/selection-residency-{index}.json").write_text(
        json.dumps(residency, sort_keys=True) + "\n", encoding="utf-8")
PY
  touch /shared/automatic-planning-ready
else
  wait_file /shared/automatic-planning-ready 18000
fi

  required_gpu_mib=$(/opt/venv/bin/python - "$rank" <<'PY'
import json
import sys
from pathlib import Path
manifest = json.loads(
    Path("/shared/automatic-planning.json").read_text(encoding="utf-8"))
print(int(manifest["stages"][int(sys.argv[1])]["requiredGpuMiB"]))
PY
  )
  # Spec175 deployment uses the ONNX Runtime CUDA backend.  The final SIF is
  # deliberately free of PyTorch/Transformers; keeping this explicit prevents
  # an old Qwen Transformers campaign from being launched by accident.
  provider_runtime=qwen-onnx
  provider_runtime_args=(
    --lazy-qwen-load
    --device cuda:0
    --require-cuda
    --require-onnx-runtime
    --selection-dataflow-v2
    --selection-model-type "$selection_model_type"
    --selection-gpu-capacity-mib 32760
    --selection-offered-gpu-mib "$required_gpu_mib"
    --selection-offer-lease-ms "$selection_offer_lease_ms"
    --selection-max-prepare-ms "$SPEC162_REQUEST_TIMEOUT_MS"
    --selection-residency-ttl-ms 7200000
    --selection-wal-path "/scratch/selection-${rank}.wal"
    --selection-storage-key-file "/scratch/selection-storage-${rank}.key"
    --selection-signing-key-file "/scratch/selection-offer-${rank}.key"
    --selection-residency-json "/shared/selection-residency-${rank}.json"
    --selection-model-cache-dir "/scratch/model-cache-${rank}"
    --repo-client-state-root "/scratch/operator-state/repo-client-${rank}"
  )
  if test "${SPEC175_LOCAL_ARTIFACTS:-0}" = 1; then
    test -n "${SPEC175_LOCAL_STAGE_PATH:-}"
    test -r "$SPEC175_LOCAL_STAGE_PATH"
    provider_runtime_args+=(
      --selection-local-artifact
      "$role=$SPEC175_LOCAL_STAGE_PATH"
    )
  else
    provider_runtime_args+=(
      --selection-repo-registration /shared/repo-registration.json
    )
  fi
else
  provider_runtime=fake
  provider_runtime_args=()
fi

/opt/venv/bin/python /source/llm_pipeline/provider.py \
  --config "$SPEC162_POLICY" \
  --generated-policy-dir "/scratch/generated/provider-${rank}" \
  --group /NDNSF-DistributeInference/example/group \
  --provider-id "$provider_id" \
  --roles "$role" \
  --runtime "$provider_runtime" \
  --stages 3 \
  --handler-workers 2 \
  --compute-delay-ms 0 \
  --provider-identity "$provider_identity" \
  --bootstrap-token-file "/scratch/provider-bootstrap-${rank}.token" \
  "${provider_runtime_args[@]}" \
  > "/scratch/log/provider-${rank}.log" 2>&1 &
provider_pid=$!
pids+=("$provider_pid")
provider_ready_wait_s="${SPEC162_PROVIDER_READY_WAIT_S:-7200}"
case "$provider_ready_wait_s" in
  ''|*[!0-9]*) echo "SPEC162_PROVIDER_READY_WAIT_S_INVALID" >&2; exit 2 ;;
esac
for _ in $(seq 1 "$((provider_ready_wait_s * 2))"); do
  grep -q LLM_PIPELINE_PROVIDER_READY "/scratch/log/provider-${rank}.log" &&
    break
  kill -0 "$provider_pid" 2>/dev/null || exit 5
  sleep 0.5
done
grep -q LLM_PIPELINE_PROVIDER_READY "/scratch/log/provider-${rank}.log"
rm -f "/scratch/provider-bootstrap-${rank}.token"
touch "/shared/provider-started-${rank}"
wait_all provider-started

# Provider-started is only a process/handler barrier.  The request below is
# the first planning trigger; certificates are fetched on demand from each
# runtime CertificatePublisher during Data validation, and model publication
# happens after ACK_CLOSED.

if test "$rank" -eq 0; then
  touch /shared/user-started
  user_rc=0
  : > /scratch/log/user.log
  if test "$control_plane_canary" = 1; then
    /opt/venv/bin/python /source/llm_pipeline/user.py \
      --config "$SPEC162_POLICY" \
      --generated-policy-dir /scratch/generated/user \
      --group /NDNSF-DistributeInference/example/group \
      --runtime fake \
      --prompt spec168-selection-control-plane-canary \
      --request-gate-output /shared/request-gate-open.json \
      --request-id "$SPEC162_SMOKE_SUBMISSION_ID" \
      --stages 3 \
      --warmup-requests 0 \
      --measured-requests 1 \
      --max-new-tokens 1 \
      --ack-timeout-ms "$ack_timeout_ms" \
      --timeout-ms 60000 \
      --app-state-root /scratch/app-state \
      --test-only-allow-ephemeral-app-state \
      >> /scratch/log/user.log 2>&1 || user_rc=$?
  else
    /opt/venv/bin/python /source/llm_pipeline/user.py \
    --config "$SPEC162_POLICY" \
    --generated-policy-dir /scratch/generated/user \
    --group /NDNSF-DistributeInference/example/group \
    --runtime qwen-onnx \
    --generation-campaign-manifest "$SPEC162_SMOKE_CAMPAIGN" \
    --automatic-planning-manifest /shared/automatic-planning.json \
    --selection-offer-key-map /shared/selection-offer-key-map.json \
    --selection-cache-max-age-ms 7200000 \
    --request-gate-output /shared/request-gate-open.json \
    --generation-jsonl /scratch/log/generation-raw.jsonl \
    --qwen-tokenizer-dir "$SPEC162_ARTIFACT_DIR/qwen-onnx-tokenizer" \
    --qwen-stage-manifest "$SPEC162_ARTIFACT_DIR/stage-manifest.json" \
    --repo-registration-output /shared/repo-registration.json \
    --repo-user /NDNSF-DistributeInference/example/user \
    --repo-bootstrap-token-file /scratch/repo-user-bootstrap.token \
    --repo-object-prefix \
      "/NDNSF-DistributeInference/example/user/NDNSF-DISTRIBUTED-REPO/OBJECT/QWEN36" \
    --request-id "$SPEC162_SMOKE_SUBMISSION_ID" \
    --model-identity-digest "$model_identity_digest" \
    --workload-digest "$workload_digest" \
    --stages 3 \
    --max-new-tokens 64 \
    --ack-timeout-ms "$ack_timeout_ms" \
    --timeout-ms "$SPEC162_REQUEST_TIMEOUT_MS" \
    --app-state-root /scratch/app-state \
    --test-only-allow-ephemeral-app-state \
      >> /scratch/log/user.log 2>&1 || user_rc=$?
  fi
  printf '%s\n' "$user_rc" > /scratch/log/user-exit.txt
  rm -f /shared/selection-offer-0.key \
    /shared/selection-offer-1.key \
    /shared/selection-offer-2.key \
    /shared/selection-offer-key-map.json
  touch /shared/user-done
  test "$user_rc" -eq 0
else
  wait_file /shared/user-started 12000
  wait_file /shared/user-done 30000
fi

touch "/shared/rank-complete-${rank}"
