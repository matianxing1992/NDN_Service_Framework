#!/bin/bash
set -Eeuo pipefail
: "${SPEC167_RANK:?}"
: "${SPEC167_PORT:?}"

rank=$SPEC167_RANK
pids=()
cleanup()
{
  rc=$?
  trap - EXIT INT TERM
  touch /shared/preflight/stop 2>/dev/null || true
  for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  nfdc status report > /scratch/log/nfd-status-exit.txt 2>/dev/null || true
  if test -n "${nfd_pid:-}"; then kill "$nfd_pid" 2>/dev/null || true; fi
  wait "${nfd_pid:-}" 2>/dev/null || true
  exit "$rc"
}
trap cleanup EXIT INT TERM

wait_file()
{
  local path=$1 attempts="${2:-3000}"
  for _ in $(seq 1 "$attempts"); do
    test -f "$path" && return 0
    sleep 0.1
  done
  echo "BARRIER_TIMEOUT:$path" >&2
  return 5
}
wait_file_or_pid()
{
  local path=$1 pid=$2 attempts="${3:-3000}"
  for _ in $(seq 1 "$attempts"); do
    test -f "$path" && return 0
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid"
      return $?
    fi
    sleep 0.1
  done
  echo "PROGRESS_TIMEOUT:$path" >&2
  return 5
}
run_nfdc() { timeout 30s nfdc "$@"; }

export NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock
export LD_LIBRARY_PATH="/opt/ndn-base/lib:/opt/ndnsf-app/lib:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/source/Experiments:/source/pythonWrapper:/source/NDNSF-DistributedRepo/pythonWrapper:/opt/ndnsf-app/python"
export PYTHONUNBUFFERED=1

/opt/venv/bin/python - <<'PY' > /scratch/log/import-probe.txt
import ndnsf
import NDNSF_DistributedRepo_Artifact_Itiger as runner
assert len(runner.build_schedule()) == 60
print("SPEC167_IMPORT_PASS")
PY
command -v nfd nfdc > /scratch/log/runtime-commands.txt
/opt/venv/bin/python - <<'PY' > /scratch/log/tcp-ceiling-probe.txt
import socket
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
sock.close()
print("SPEC167_TCP_CEILING_CAPABILITY_PASS")
PY

nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
for _ in $(seq 1 300); do
  test -S /scratch/run/nfd.sock && run_nfdc status >/dev/null 2>&1 && break
  kill -0 "$nfd_pid" 2>/dev/null || exit 5
  sleep 0.1
done
test -S /scratch/run/nfd.sock
touch "/shared/nfd-ready-${rank}"
wait_file /shared/nfd-ready-0
wait_file /shared/nfd-ready-1

peer_rank=$((1-rank))
peer_ip=$(<"/shared/node-${peer_rank}/ipv4.txt")
uri="tcp4://${peer_ip}:${SPEC167_PORT}"
run_nfdc face create "$uri" > /scratch/log/face-create.txt
if test "$rank" -eq 0; then
  run_nfdc route add /spec164/repo-cold "$uri" origin 65 cost 0
else
  run_nfdc route add /spec164/raw "$uri" origin 65 cost 0
fi
run_nfdc status report > /scratch/log/nfd-status-ready.txt
touch "/shared/routes-ready-${rank}"
wait_file /shared/routes-ready-0
wait_file /shared/routes-ready-1
sleep 2

coord=/shared/preflight
data=/scratch/data/preflight
mkdir -p "$coord"
if test "$rank" -eq 0; then
  /opt/venv/bin/python /source/Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py \
    --role prepare --coord-dir "$coord" --data-dir "$data" \
    --payload-size 67108864 > /scratch/log/prepare.txt
  /opt/venv/bin/python /source/Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py \
    --role producer --coord-dir "$coord" --data-dir "$data" \
    --subject raw-segmented-ndn --timeout-seconds 300 \
    > /scratch/log/producer.txt 2>&1 &
  producer_pid=$!
  pids+=("$producer_pid")
  wait_file_or_pid "$coord/preflight.store-ready.json" "$producer_pid" 3000
  touch "$coord/preflight.serve-cold"
  wait_file "$coord/preflight.producer-ready.json" 3000
  /opt/venv/bin/python /source/Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py \
    --role cold-consumer --coord-dir "$coord" --data-dir "$data" \
    --result-name preflight --timeout-seconds 300 \
    > /scratch/log/cold-consumer.txt 2>&1
  wait_file "$coord/preflight.json" 3000
  touch "$coord/stop"
  wait "$producer_pid"
  python3 - "$coord" <<'PY'
import json, pathlib, sys
root=pathlib.Path(sys.argv[1])
result=json.loads((root/'preflight.json').read_text())
cold=json.loads((root/'preflight.cold.json').read_text())
assert result['status']=='SUCCESS' and cold['status']=='SUCCESS'
assert cold['destination'].startswith('/scratch/data/')
print('SPEC167_CROSS_NODE_RAW_PASS')
PY
  touch /shared/preflight-complete
else
  wait_file "$coord/benchmark-producer.ready" 3000
  /opt/venv/bin/python /source/Experiments/NDNSF_DistributedRepo_Artifact_Itiger.py \
    --role consumer --coord-dir "$coord" --data-dir "$data" \
    --subject raw-segmented-ndn --result-name preflight \
    --timeout-seconds 300 > /scratch/log/consumer.txt 2>&1
  wait_file /shared/preflight-complete 3000
fi

touch "/shared/rank-complete-${rank}"
