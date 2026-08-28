#!/bin/bash
set -Eeuo pipefail
: "${SLURM_PROCID:?}"
: "${SPEC167_SHARED:?}"
: "${SPEC167_SIF:?}"
: "${SPEC167_SOURCE:?}"
: "${SPEC167_PORT:?}"

rank=$SLURM_PROCID
run_name="${SPEC167_RUN_NAME:-preflight}"
if test -n "${SPEC167_PEER_HOST:-}"; then
  peer_host=$SPEC167_PEER_HOST
elif test "$rank" -eq 0; then
  peer_host=$(getent ahostsv4 itiger08 | awk 'NR==1{print $1}')
else
  peer_host=$(getent ahostsv4 itiger07 | awk 'NR==1{print $1}')
fi
test -n "$peer_host"
base="${SLURM_TMPDIR:-/tmp/${USER}}"
scratch="${base}/spec167-${SLURM_JOB_ID}-${run_name}-rank-${rank}"
test ! -e "$scratch"
mkdir -p "$scratch/run" "$scratch/home" "$scratch/log" "$scratch/data"

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  if test "${SPEC167_MODE:-preflight}" = formal; then
    log_dest="$SPEC167_SHARED/runs/$run_name/node-${rank}"
  else
    log_dest="$SPEC167_SHARED/node-${rank}"
  fi
  mkdir -p "$log_dest" 2>/dev/null || true
  cp -a "$scratch/log/." "$log_dest/" 2>/dev/null || true
  rm -rf -- "$scratch"
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$SPEC167_SHARED/node-${rank}"
hostname > "$SPEC167_SHARED/node-${rank}/hostname.txt"
hostname -I | awk '{print $1}' > "$SPEC167_SHARED/node-${rank}/ipv4.txt"
findmnt -T "$scratch" -o TARGET,SOURCE,FSTYPE,OPTIONS \
  > "$SPEC167_SHARED/node-${rank}/scratch-mount.txt"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e "s|@@TCP_PORT@@|${SPEC167_PORT}|g" \
    -e "s|@@NODE_RANK@@|${rank}|g" \
    "$SPEC167_SOURCE/specs/167-itiger-repo-throughput/jobs/nfd.conf.in" \
    > "$scratch/nfd.conf"

apptainer exec --cleanenv --containall \
  --home "$scratch/home:/home/${USER}" \
  --bind "$scratch:/scratch:rw" \
  --bind "$SPEC167_SHARED:/shared:rw" \
  --bind "$SPEC167_SOURCE:/source:ro" \
  --env "SPEC167_RANK=${rank}" \
  --env "SPEC167_PORT=${SPEC167_PORT}" \
  --env "SPEC167_MODE=${SPEC167_MODE:-preflight}" \
  --env "SPEC167_RUN_NAME=${run_name}" \
  --env "SPEC167_SUBJECT=${SPEC167_SUBJECT:-raw-segmented-ndn}" \
  --env "SPEC167_PAYLOAD_SIZE=${SPEC167_PAYLOAD_SIZE:-67108864}" \
  --env "SPEC167_TCP_PORT=${SPEC167_TCP_PORT:-26369}" \
  --env "SPEC167_DURATION_SECONDS=${SPEC167_DURATION_SECONDS:-60}" \
  --env "SPEC167_PEER_HOST=${peer_host}" \
  --env "SPEC167_STARTUP_DELAY_SECONDS=${SPEC167_STARTUP_DELAY_SECONDS:-0}" \
  "$SPEC167_SIF" \
  "/source/specs/167-itiger-repo-throughput/jobs/$([ "${SPEC167_MODE:-preflight}" = formal ] && echo campaign-rank-inner.sh || echo rank-inner.sh)"
