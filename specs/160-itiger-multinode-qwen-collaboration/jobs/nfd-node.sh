#!/bin/bash
set -Eeuo pipefail
rank=$1
port=$2
shared=$3
: "${SLURM_JOB_ID:?}"

host_scratch="/tmp/${USER}/spec160-probe-${SLURM_JOB_ID}"
rm -rf "$host_scratch"
mkdir -p "$host_scratch/run" "$host_scratch/home" "$host_scratch/log"
sed -e "s|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g" \
    -e "s|@@PORT@@|${port}|g" \
    "$shared/source/nfd.conf.in" > "$host_scratch/nfd.conf"

cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  if test -n "${nfd_pid:-}"; then kill "$nfd_pid" 2>/dev/null || true; fi
  wait "${nfd_pid:-}" 2>/dev/null || true
  cp -a "$host_scratch/log/." "$shared/node-${rank}/" 2>/dev/null || true
  rm -rf "$host_scratch"
  exit "$rc"
}
trap cleanup EXIT INT TERM

mkdir -p "$shared/node-${rank}"
hostname > "$shared/node-${rank}/hostname.txt"
hostname -I | awk '{print $1}' > "$shared/node-${rank}/ipv4.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$shared/node-${rank}/gpu.csv"
test "$(wc -l < "$shared/node-${rank}/gpu.csv")" -eq 1

apptainer exec --nv --cleanenv --containall \
  --home "$host_scratch/home:/home/${USER}" \
  --bind "$host_scratch:/scratch:rw" \
  --env "NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock,HOME=/home/${USER}" \
  "$SPEC160_SIF" bash -lc \
  'nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
   echo $! > /scratch/run/nfd.pid
   for i in $(seq 1 100); do
     test -S /scratch/run/nfd.sock && nfdc status >/dev/null 2>&1 && break
     sleep 0.1
   done
   test -S /scratch/run/nfd.sock
   nfdc status report > /scratch/log/nfd-status-initial.txt
   touch /scratch/run/ready
   while test ! -f /scratch/run/stop; do sleep 0.2; done
   nfdc status report > /scratch/log/nfd-status-final.txt
   kill $(cat /scratch/run/nfd.pid) 2>/dev/null || true
   wait $(cat /scratch/run/nfd.pid) 2>/dev/null || true' &
nfd_pid=$!

for _ in $(seq 1 200); do
  test -f "$host_scratch/run/ready" && break
  kill -0 "$nfd_pid" 2>/dev/null || exit 5
  sleep 0.1
done
test -f "$host_scratch/run/ready"
touch "$shared/node-${rank}/ready"
while test ! -f "$shared/stop"; do
  kill -0 "$nfd_pid" 2>/dev/null || exit 5
  sleep 0.2
done
touch "$host_scratch/run/stop"
wait "$nfd_pid"
