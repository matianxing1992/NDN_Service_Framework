#!/bin/bash
set -Eeuo pipefail
: "${SLURM_PROCID:?}"
: "${SPEC166_SHARED:?}"
: "${SPEC166_SIF:?}"
: "${SPEC166_SOURCE:?}"
: "${SPEC166_PORT:?}"

rank=$SLURM_PROCID
base="${SLURM_TMPDIR:-/tmp/${USER}}"
scratch="${base}/spec166-qwen-${SLURM_JOB_ID}-rank-${rank}"
test ! -e "$scratch"
mkdir -p "$scratch/run" "$scratch/home" "$scratch/log" "$scratch/generated"
cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  cp -a "$scratch/log/." "$SPEC166_SHARED/node-${rank}/" 2>/dev/null || true
  rm -rf "$scratch"
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$SPEC166_SHARED/node-${rank}"
hostname > "$SPEC166_SHARED/node-${rank}/hostname.txt"
hostname -I | awk '{print $1}' > "$SPEC166_SHARED/node-${rank}/ipv4.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$SPEC166_SHARED/node-${rank}/gpu.csv"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e "s|@@PORT@@|${SPEC166_PORT}|g" \
    "$SPEC166_SOURCE/nfd.conf.in" > "$scratch/nfd.conf"

apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/${USER}" \
  --bind "$scratch:/scratch:rw" \
  --bind "$SPEC166_SHARED:/shared:rw" \
  --bind "$SPEC166_SOURCE:/source:ro" \
  --bind "$SPEC166_INPUT:/input:ro" \
  --env "SPEC166_RANK=${rank}" \
  --env "SPEC166_PORT=${SPEC166_PORT}" \
  --env "SPEC166_RUN_ID=${SPEC166_RUN_ID}" \
  "$SPEC166_SIF" /source/multinode-rank-inner.sh
