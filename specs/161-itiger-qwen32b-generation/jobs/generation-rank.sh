#!/bin/bash
set -Eeuo pipefail

: "${SLURM_PROCID:?}"
: "${SPEC161_SHARED:?}"
: "${SPEC161_SIF:?}"
: "${SPEC161_PORT:?}"
: "${SPEC161_ARTIFACT_DIR:?}"
: "${SPEC161_POLICY:?}"
: "${SPEC161_SMOKE_CAMPAIGN:?}"

rank=$SLURM_PROCID
base="${SLURM_TMPDIR:-/tmp/${USER}}"
scratch="${base%/}/tma1/ndnsf-di/spec161-smoke-${SLURM_JOB_ID}-rank-${rank}"
case "$scratch" in
  /scratch/tma1/ndnsf-di/spec161-smoke-*|/tmp/tma1/ndnsf-di/spec161-smoke-*) ;;
  *)
    if test -z "${SLURM_TMPDIR:-}" ||
       test "$scratch" != \
         "${SLURM_TMPDIR%/}/tma1/ndnsf-di/spec161-smoke-${SLURM_JOB_ID}-rank-${rank}"; then
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
  cp -a "$scratch/log/." "$SPEC161_SHARED/node-${rank}/" 2>/dev/null || true
  find "$scratch" -mindepth 1 -delete 2>/dev/null || true
  rmdir -- "$scratch" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$SPEC161_SHARED/node-${rank}"
hostname > "$SPEC161_SHARED/node-${rank}/hostname.txt"
hostname -I | awk '{print $1}' > "$SPEC161_SHARED/node-${rank}/ipv4.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$SPEC161_SHARED/node-${rank}/gpu.csv"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e "s|@@PORT@@|${SPEC161_PORT}|g" \
    "$SPEC161_SHARED/source/jobs/nfd.conf.in" > "$scratch/nfd.conf"

apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/${USER}" \
  --bind "$scratch:/scratch:rw" \
  --bind "$SPEC161_SHARED:/shared:rw" \
  --bind "$SPEC161_SHARED/source:/source:ro" \
  --bind "$SPEC161_ARTIFACT_DIR:$SPEC161_ARTIFACT_DIR:ro" \
  --env "SPEC161_RANK=${rank}" \
  --env "SPEC161_PORT=${SPEC161_PORT}" \
  --env "SPEC161_POLICY=${SPEC161_POLICY}" \
  --env "SPEC161_SMOKE_CAMPAIGN=${SPEC161_SMOKE_CAMPAIGN}" \
  "$SPEC161_SIF" bash /source/jobs/generation-rank-inner.sh
