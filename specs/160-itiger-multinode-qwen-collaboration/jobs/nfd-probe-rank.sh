#!/bin/bash
set -Eeuo pipefail
: "${SLURM_PROCID:?}"
: "${SPEC160_SHARED:?}"
: "${SPEC160_SIF:?}"
: "${SPEC160_PORT:?}"

rank=$SLURM_PROCID
base="${SLURM_TMPDIR:-/tmp/${USER}}"
scratch="${base}/spec160-probe-${SLURM_JOB_ID}-rank-${rank}"
rm -rf "$scratch"
mkdir -p "$scratch/run" "$scratch/home" "$scratch/log"
cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  cp -a "$scratch/log/." "$SPEC160_SHARED/node-${rank}/" 2>/dev/null || true
  rm -rf "$scratch"
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$SPEC160_SHARED/node-${rank}"
hostname > "$SPEC160_SHARED/node-${rank}/hostname.txt"
hostname -I | awk '{print $1}' > "$SPEC160_SHARED/node-${rank}/ipv4.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$SPEC160_SHARED/node-${rank}/gpu.csv"
test "$(wc -l < "$SPEC160_SHARED/node-${rank}/gpu.csv")" -eq 1
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e "s|@@PORT@@|${SPEC160_PORT}|g" \
    "$SPEC160_SHARED/source/nfd.conf.in" > "$scratch/nfd.conf"

apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/${USER}" \
  --bind "$scratch:/scratch:rw" \
  --bind "$SPEC160_SHARED:/shared:rw" \
  --bind "$SPEC160_SHARED/source:/source:ro" \
  --env "NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock" \
  --env "SPEC160_RANK=${rank}" \
  --env "SPEC160_PORT=${SPEC160_PORT}" \
  --env "SLURM_JOB_ID=${SLURM_JOB_ID}" \
  "$SPEC160_SIF" /source/nfd-probe-rank-inner.sh
