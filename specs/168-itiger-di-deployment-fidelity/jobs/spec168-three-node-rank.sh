#!/bin/bash
set -Eeuo pipefail

: "${SLURM_PROCID:?}"
: "${SPEC168_SHARED:?}"
: "${SPEC168_SOURCE_DIR:?}"
: "${SPEC168_SIF:?}"
: "${SPEC168_PORT:?}"
: "${SPEC168_POLICY:?}"
: "${SPEC168_REQUEST_ID:?}"
control_plane_canary="${SPEC168_CONTROL_PLANE_CANARY:-0}"
case "$control_plane_canary" in
  0)
    : "${SPEC168_ARTIFACT_DIR:?}"
    : "${SPEC168_GENERATION_CAMPAIGN:?}"
    : "${SPEC168_MODEL_IDENTITY_DIGEST:?}"
    : "${SPEC168_WORKLOAD_DIGEST:?}"
    ;;
  1) ;;
  *) echo "SPEC168_CONTROL_PLANE_CANARY_INVALID:$control_plane_canary" >&2; exit 2 ;;
esac

rank=$SLURM_PROCID
if test -n "${SLURM_TMPDIR:-}"; then
  scratch="${SLURM_TMPDIR%/}/ndnsf-di/spec168-${SLURM_JOB_ID}-rank-${rank}"
else
  scratch="/tmp/${USER}/ndnsf-di/spec168-${SLURM_JOB_ID}-rank-${rank}"
fi
case "$scratch" in
  /scratch/tma1/ndnsf-di/spec168-*|/tmp/tma1/ndnsf-di/spec168-*) ;;
  *)
    if test -z "${SLURM_TMPDIR:-}" ||
       test "$scratch" != \
         "${SLURM_TMPDIR%/}/ndnsf-di/spec168-${SLURM_JOB_ID}-rank-${rank}"; then
      echo "SPEC168_UNSAFE_RANK_SCRATCH:${scratch}" >&2
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
  if test "$rc" -ne 0; then
    printf 'rank=%s rc=%s boundary=outer-wrapper\n' "$rank" "$rc" \
      > "$SPEC168_SHARED/outer-rank-failed-${rank}.txt" 2>/dev/null || true
    touch "$SPEC168_SHARED/rank-abort" 2>/dev/null || true
  fi
  mkdir -p "$SPEC168_SHARED/node-${rank}"
  cp -a "$scratch/log/." "$SPEC168_SHARED/node-${rank}/" 2>/dev/null || true
  chmod -R u+w "$scratch" 2>/dev/null || true
  find "$scratch" -mindepth 1 -delete 2>/dev/null || true
  rmdir -- "$scratch" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$SPEC168_SHARED/node-${rank}"
hostname > "$SPEC168_SHARED/node-${rank}/hostname.txt"
hostname -I | awk '{print $1}' > "$SPEC168_SHARED/node-${rank}/ipv4.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$SPEC168_SHARED/node-${rank}/gpu.csv"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e "s|@@PORT@@|${SPEC168_PORT}|g" \
    "$SPEC168_SOURCE_DIR/jobs/nfd.conf.in" > "$scratch/nfd.conf"
cp "$SPEC168_POLICY" "$scratch/policy.yaml"
chmod 0400 "$scratch/policy.yaml"

for directory in \
  "$SPEC168_SOURCE_DIR/di/ndnsf_distributed_inference" \
  "$SPEC168_SOURCE_DIR/ndnsf/ndnsf" \
  "$SPEC168_SOURCE_DIR/repo/py_repoclient" \
  "$SPEC168_SOURCE_DIR/llm_pipeline"; do
  test -d "$directory"
done
test -r "$SPEC168_SOURCE_DIR/compat/spec162/generation-rank-inner.sh"

readonly request_timeout_ms="${SPEC168_REQUEST_TIMEOUT_MS:-3600000}"
readonly ack_timeout_ms="${SPEC168_ACK_TIMEOUT_MS:-120000}"
readonly selection_lease_ms="${SPEC168_SELECTION_OFFER_LEASE_MS:-$request_timeout_ms}"
readonly face_scheme="${SPEC168_FACE_SCHEME:-tcp4}"
case "$face_scheme" in tcp4|udp4) ;; *) exit 2 ;; esac

model_bind_args=()
model_env_args=()
if test "$control_plane_canary" = 0; then
  model_bind_args=(--bind "$SPEC168_ARTIFACT_DIR:$SPEC168_ARTIFACT_DIR:ro")
  model_env_args=(
    --env "SPEC168_GENERATION_CAMPAIGN=/shared/$(basename "$SPEC168_GENERATION_CAMPAIGN")"
    --env "SPEC168_ARTIFACT_DIR=${SPEC168_ARTIFACT_DIR}"
    --env "SPEC168_MODEL_IDENTITY_DIGEST=${SPEC168_MODEL_IDENTITY_DIGEST}"
    --env "SPEC168_WORKLOAD_DIGEST=${SPEC168_WORKLOAD_DIGEST}"
  )
fi

apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/${USER}" \
  --bind "$scratch:/scratch:rw" \
  --bind "$SPEC168_SHARED:/shared:rw" \
  --bind "$SPEC168_SOURCE_DIR:/source:ro" \
  "${model_bind_args[@]}" \
  --env "SPEC168_RANK=${rank}" \
  --env "SPEC168_PORT=${SPEC168_PORT}" \
  --env "SPEC168_POLICY=/scratch/policy.yaml" \
  --env "SPEC168_REQUEST_ID=${SPEC168_REQUEST_ID}" \
  --env "SPEC168_REQUEST_TIMEOUT_MS=${request_timeout_ms}" \
  --env "SPEC168_ACK_TIMEOUT_MS=${ack_timeout_ms}" \
  --env "SPEC168_SELECTION_OFFER_LEASE_MS=${selection_lease_ms}" \
  --env "SPEC168_FACE_SCHEME=${face_scheme}" \
  --env "SPEC168_CONTROL_PLANE_CANARY=${control_plane_canary}" \
  "${model_env_args[@]}" \
  --env "PYTHONPATH=/opt/ndnsf-app/python" \
  --env "NDNSF_PIPELINE_MARKER_LOG=/scratch/log/provider-markers-${rank}.log" \
  --env "NDNSF_ARTIFACT_DEBUG_ERRORS=${NDNSF_ARTIFACT_DEBUG_ERRORS:-0}" \
  "$SPEC168_SIF" bash /source/jobs/spec168-three-node-rank-inner.sh
